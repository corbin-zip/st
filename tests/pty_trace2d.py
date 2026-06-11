#!/usr/bin/env python3
"""Capture ground-truth byte traces of interactive shells reacting to 2D PTY
resizes (cols AND rows changing together, like corner-dragging a window).

Based on pty_trace.py. Spawns the shell on a fresh PTY as session leader with
the slave as controlling tty, types two echo commands, then performs a resize
schedule via TIOCSWINSZ (delivering real SIGWINCH) while recording all output.

Trace format (one event per line):
  R <cols> <rows>     resize performed
  O <hex>             raw output bytes, lowercase hex, chunked as read() returned

Usage: pty_trace2d.py [scenario ...] [--shell zsh|bash]
"""

import fcntl
import os
import random
import select
import signal
import struct
import sys
import termios
import time

OUTDIR = "/tmp/st-reflow-tests"
BASHRC = os.path.join(OUTDIR, "bashrc")
SEED = 1337

SHELLS = {
    "zsh": ["zsh", "-i"],
    "bash": ["bash", "--rcfile", BASHRC, "-i"],
}


# ---------------------------------------------------------------------------
# Scenario builders: each returns a list of (cols, rows, delay_after) steps.
# ---------------------------------------------------------------------------

def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def scen_diagonal(delay=0.03):
    """(80,24) -> (3,3) stepping both dims together, then back."""
    steps = []
    c, r = 80, 24
    while c > 3 or r > 3:
        c = max(3, c - 3)
        r = max(3, r - 1)
        steps.append((c, r, delay))
    while c < 80 or r < 24:
        c = min(80, c + 3)
        r = min(24, r + 1)
        steps.append((c, r, delay))
    return steps


def scen_diag_fast():
    return scen_diagonal(delay=0.005)


def scen_diag_storm():
    return scen_diagonal(delay=0.0)


def scen_wiggle(delay=0.03, cycles=10):
    """Shrink to ~(4,4), bounce randomly in (3..12, 3..12), then back."""
    rng = random.Random(SEED)
    steps = []
    # shrink diagonally to the corner
    c, r = 80, 24
    while c > 4 or r > 4:
        c = max(4, c - 4)
        r = max(4, r - 1)
        steps.append((c, r, delay))
    # bounce around the corner
    for _ in range(cycles * 4):
        c = rng.randint(3, 12)
        r = rng.randint(3, 12)
        steps.append((c, r, delay))
    # grow back
    while c < 80 or r < 24:
        c = min(80, c + 4)
        r = min(24, r + 1)
        steps.append((c, r, delay))
    return steps


def scen_wiggle_storm():
    return scen_wiggle(delay=0.0, cycles=12)


def scen_wiggle_slow():
    return scen_wiggle(delay=0.08, cycles=8)


def scen_jumpy(n=150):
    """Random walk over cols [2,40], rows [3,24]; storm bursts then pauses."""
    rng = random.Random(SEED + 1)
    steps = []
    c, r = 80, 24
    for i in range(n):
        c = clamp(c + rng.randint(-6, 6), 2, 40)
        r = clamp(r + rng.randint(-4, 4), 3, 24)
        delay = 0.005 if (i // 10) % 2 == 0 else 0.1
        steps.append((c, r, delay))
    steps.append((80, 24, 0.2))
    return steps


def scen_jumpy_storm(n=150):
    rng = random.Random(SEED + 2)
    steps = []
    c, r = 80, 24
    for i in range(n):
        c = clamp(c + rng.randint(-10, 10), 2, 40)
        r = clamp(r + rng.randint(-6, 6), 2, 24)
        # 20-step storms with no delay, then a breath so the shell catches up
        delay = 0.0 if i % 25 else 0.15
        steps.append((c, r, delay))
    steps.append((80, 24, 0.2))
    return steps


def scen_rowschurn(delay=0.02):
    """Cols fixed 6 (narrower than prompt), rows bouncing 24 <-> 4."""
    steps = [(6, 24, 0.1)]
    for _ in range(8):
        for r in range(23, 3, -2):
            steps.append((6, r, delay))
        for r in range(5, 25, 2):
            steps.append((6, r, delay))
    steps.append((80, 24, 0.2))
    return steps


def scen_rowschurn_storm():
    return scen_rowschurn(delay=0.0)


def scen_tinybox(delay=0.02):
    """Live in the 2..10 x 2..10 box for a long random bounce."""
    rng = random.Random(SEED + 3)
    steps = [(10, 10, 0.1)]
    for _ in range(120):
        steps.append((rng.randint(2, 10), rng.randint(2, 10), delay))
    steps.append((80, 24, 0.2))
    return steps


def scen_tinybox_storm():
    rng = random.Random(SEED + 4)
    steps = [(10, 10, 0.1)]
    for i in range(160):
        steps.append((rng.randint(2, 10), rng.randint(2, 10),
                      0.0 if i % 30 else 0.12))
    steps.append((80, 24, 0.2))
    return steps


def scen_strand1():
    """REPRODUCES the joined-prompt bug.

    Cycle: settle at medium width (zsh redraw leaves a small stale vpos) ->
    big column jump down (the lagged redraw moves up too few rows and
    strands prompt-head fragments above) -> immediate row crush (pushes the
    stranded rows into scrollback, out of reach of later \\e[nA\\e[J wipes)
    -> widen at tiny rows (reflow joins the wrapped fragments).
    """
    steps = []
    for _ in range(6):
        steps.append((24, 24, 0.25))
        steps.append((4, 24, 0.08))
        steps.append((4, 4, 0.0))
        steps.append((4, 3, 0.08))
        steps.append((30, 3, 0.08))
        steps.append((30, 24, 0.15))
    steps.append((80, 24, 0.3))
    return steps


def scen_strand2():
    """REPRODUCES: like strand1 but ends at a small size."""
    return scen_strand1()[:-1] + [(5, 4, 0.1)]


def scen_strand3():
    """REPRODUCES: column shrink in zero-delay jumps so the lagged redraw
    lands at the final tiny size; otherwise like strand1."""
    steps = []
    for _ in range(8):
        steps.append((36, 24, 0.25))
        steps.append((18, 24, 0.0))
        steps.append((9, 24, 0.0))
        steps.append((4, 24, 0.06))
        steps.append((4, 6, 0.0))
        steps.append((4, 3, 0.06))
        steps.append((60, 3, 0.06))
        steps.append((60, 24, 0.2))
    steps.append((80, 24, 0.3))
    return steps


def scen_strand4():
    """Opposite staleness direction (settle narrow, jump wide): does NOT
    reproduce — overshooting \\e[nA only over-erases (eaten, accepted)."""
    steps = []
    for _ in range(6):
        steps.append((5, 24, 0.3))
        steps.append((40, 24, 0.08))
        steps.append((6, 24, 0.06))
        steps.append((6, 5, 0.0))
        steps.append((6, 4, 0.08))
        steps.append((40, 4, 0.08))
        steps.append((40, 24, 0.15))
    steps.append((80, 24, 0.3))
    return steps


SCENARIOS = {
    "diag": scen_diagonal,
    "diagfast": scen_diag_fast,
    "diagstorm": scen_diag_storm,
    "wiggle2d": scen_wiggle,
    "wiggle2dstorm": scen_wiggle_storm,
    "wiggle2dslow": scen_wiggle_slow,
    "jumpy": scen_jumpy,
    "jumpystorm": scen_jumpy_storm,
    "rowschurn": scen_rowschurn,
    "rowschurnstorm": scen_rowschurn_storm,
    "tinybox": scen_tinybox,
    "tinyboxstorm": scen_tinybox_storm,
    "strand1": scen_strand1,
    "strand2": scen_strand2,
    "strand3": scen_strand3,
    "strand4": scen_strand4,
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


def drain_now(master, events):
    """Non-blocking drain of whatever is already pending."""
    while True:
        try:
            r, _, _ = select.select([master], [], [], 0)
        except InterruptedError:
            continue
        if not r:
            return True
        try:
            data = os.read(master, 65536)
        except OSError:
            return False
        if not data:
            return False
        events.append(("O", data))


def run_trace(shell, argv, label, steps):
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

        for cols, rows, delay in steps:
            if not alive:
                break
            set_winsize(master, cols, rows)
            events.append(("R", cols, rows))
            if delay > 0:
                alive = read_for(master, delay, events)
            else:
                alive = drain_now(master, events)

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

    path = os.path.join(OUTDIR, "trace2d_%s_%s.txt" % (shell, label))
    with open(path, "w") as f:
        for ev in events:
            if ev[0] == "R":
                f.write("R %d %d\n" % (ev[1], ev[2]))
            else:
                f.write("O %s\n" % ev[1].hex())
    if not alive:
        print("  WARNING: %s/%s: shell exited/EOF before trace finished" % (shell, label))
    return path, events


def main():
    os.makedirs(OUTDIR, exist_ok=True)
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    shells = ["zsh"]
    if "--bash" in sys.argv[1:]:
        shells = ["bash"]
    elif "--both" in sys.argv[1:]:
        shells = ["zsh", "bash"]
    wanted = args if args else list(SCENARIOS)
    for shell in shells:
        argv = SHELLS[shell]
        for label in wanted:
            steps = SCENARIOS[label]()
            t0 = time.monotonic()
            print("running %s / %s (%d steps)..." % (shell, label, len(steps)))
            path, events = run_trace(shell, argv, label, steps)
            print("  wrote %s (%d events, %.1fs)"
                  % (path, len(events), time.monotonic() - t0))


if __name__ == "__main__":
    main()
