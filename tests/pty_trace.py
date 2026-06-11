#!/usr/bin/env python3
"""Capture ground-truth byte traces of interactive shells reacting to PTY resizes.

For each shell (zsh with real config, bash with a LARBS-like prompt) and each
drag speed (fast/med/slow), spawn the shell on a fresh PTY as session leader
with the slave as controlling tty, type a couple of commands, then "drag" the
width 79 -> 8 -> 80 one column at a time via TIOCSWINSZ on the master (which
delivers SIGWINCH to the foreground process group). All master-side output and
all resize events are recorded in order.

Trace format (one event per line):
  R <cols> <rows>     resize performed
  O <hex>             raw output bytes, lowercase hex, chunked as read() returned
"""

import fcntl
import os
import select
import signal
import struct
import sys
import termios
import time

OUTDIR = "/tmp/st-reflow-tests"
BASHRC = os.path.join(OUTDIR, "bashrc")

SHELLS = {
    "zsh": ["zsh", "-i"],
    "bash": ["bash", "--rcfile", BASHRC, "-i"],
}
DELAYS = {
    "fast": 0.005,
    "med": 0.05,
    "slow": 0.2,
}


def set_winsize(fd, cols, rows):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def spawn(argv):
    """openpty + fork; child becomes session leader with slave as ctty."""
    master, slave = os.openpty()
    set_winsize(master, 80, 24)  # initial size before the shell starts
    pid = os.fork()
    if pid == 0:
        try:
            os.close(master)
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            os.dup2(slave, 0)
            os.dup2(slave, 1)
            os.dup2(slave, 2)
            if slave > 2:
                os.close(slave)
            env = dict(os.environ)
            env["TERM"] = "st-256color"
            env.setdefault("ZDOTDIR", "/home/carbon/.config/zsh")
            env.pop("PS1", None)
            os.execvpe(argv[0], argv, env)
        except Exception as e:  # pragma: no cover
            os.write(2, ("exec failed: %r\n" % (e,)).encode())
        finally:
            os._exit(127)
    os.close(slave)
    return master, pid


def read_for(master, duration, events):
    """Drain master for `duration` seconds, appending O events. False = EOF."""
    end = time.monotonic() + duration
    while True:
        remaining = end - time.monotonic()
        if remaining <= 0:
            return True
        try:
            r, _, _ = select.select([master], [], [], min(remaining, 0.05))
        except InterruptedError:
            continue
        if r:
            try:
                data = os.read(master, 65536)
            except OSError:
                return False  # EIO: child side closed
            if not data:
                return False
            events.append(("O", data))


def run_trace(shell, argv, label, delay):
    master, pid = spawn(argv)
    events = [("R", 80, 24)]
    alive = True
    try:
        alive = read_for(master, 1.5, events)
        for cmd in (b"echo hello test\n", b"echo hi\n"):
            if not alive:
                break
            os.write(master, cmd)
            alive = read_for(master, 0.5, events)

        # the drag: 79 -> 8, then 9 -> 80, rows fixed at 24
        widths = list(range(79, 7, -1)) + list(range(9, 81))
        for cols in widths:
            if not alive:
                break
            set_winsize(master, cols, 24)
            events.append(("R", cols, 24))
            alive = read_for(master, delay, events)

        if alive:
            read_for(master, 1.5, events)
    finally:
        try:
            os.killpg(pid, signal.SIGHUP)
        except (ProcessLookupError, PermissionError):
            pass
        try:
            os.close(master)
        except OSError:
            pass
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            try:
                wpid, _ = os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                break
            if wpid == pid:
                break
            time.sleep(0.05)
        else:
            try:
                os.killpg(pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass

    path = os.path.join(OUTDIR, "trace_%s_%s.txt" % (shell, label))
    with open(path, "w") as f:
        for ev in events:
            if ev[0] == "R":
                f.write("R %d %d\n" % (ev[1], ev[2]))
            else:
                f.write("O %s\n" % ev[1].hex())
    if not alive:
        print("  WARNING: %s/%s: shell exited/EOF before trace finished" % (shell, label))
    return path, events


def decode_bytes(data):
    out = []
    for b in data:
        if b == 0x1B:
            out.append("\\e")
        elif b == 0x0D:
            out.append("\\r")
        elif b == 0x0A:
            out.append("\\n")
        elif b == 0x07:
            out.append("\\a")
        elif b == 0x08:
            out.append("\\b")
        elif b == 0x09:
            out.append("\\t")
        elif 32 <= b < 127:
            out.append(chr(b))
        else:
            out.append("\\x%02x" % b)
    return "".join(out)


def summarize(shell, label, events, fh):
    n_resize = sum(1 for e in events if e[0] == "R")
    chunks = [e[1] for e in events if e[0] == "O"]
    total = sum(len(c) for c in chunks)
    fh.write("=== trace_%s_%s ===\n" % (shell, label))
    fh.write("resize events: %d\n" % n_resize)
    fh.write("output chunks: %d (total %d bytes, min %d, max %d)\n"
             % (len(chunks), total,
                min((len(c) for c in chunks), default=0),
                max((len(c) for c in chunks), default=0)))

    # decoded peek of output emitted while 8 <= cols <= 20
    cols = 80
    narrow = []
    for e in events:
        if e[0] == "R":
            cols = e[1]
        elif 8 <= cols <= 20:
            narrow.append((cols, e[1]))
    fh.write("output while 8 <= cols <= 20: %d chunks, %d bytes\n"
             % (len(narrow), sum(len(d) for _, d in narrow)))
    fh.write("decoded peek (narrow widths):\n")
    budget = 1600
    for cols, data in narrow:
        if budget <= 0:
            fh.write("  ... (truncated)\n")
            break
        dec = decode_bytes(data)[:budget]
        budget -= len(dec)
        fh.write("  [cols=%2d] %s\n" % (cols, dec))
    fh.write("\n")


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    summary_path = os.path.join(OUTDIR, "trace_summary.txt")
    with open(summary_path, "w") as fh:
        for shell, argv in SHELLS.items():
            for label, delay in DELAYS.items():
                print("running %s / %s (delay %gs)..." % (shell, label, delay))
                t0 = time.monotonic()
                path, events = run_trace(shell, argv, label, delay)
                print("  wrote %s (%d events, %.1fs)"
                      % (path, len(events), time.monotonic() - t0))
                summarize(shell, label, events, fh)
    print("summary: %s" % summary_path)


if __name__ == "__main__":
    main()
