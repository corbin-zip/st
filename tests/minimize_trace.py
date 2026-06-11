#!/usr/bin/env python3
"""Greedy minimizer for reflow-bug PTY traces.

Removes runs of R/O events (then splits O chunks and trims their tails)
while the replay through test_st_hunt still reports joined>0 or frags>3.

Usage: minimize_trace.py <in-trace> <out-trace>
"""

import os
import re
import subprocess
import sys
import tempfile

HUNT = "/tmp/st-reflow-tests/test_st_hunt"
ENV = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")


def _raw_repro(lines):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.writelines(lines)
        path = f.name
    try:
        out = subprocess.run([HUNT, path], capture_output=True, text=True,
                             env=ENV).stdout
    finally:
        os.unlink(path)
    m = re.search(r"joined: (\d+)\s+prompt frags: (\d+)", out)
    if not m:
        return False
    joined, frags = int(m.group(1)), int(m.group(2))
    return joined > 0 or frags > 3


def reproduces(lines):
    """Valid repro: junk appears with the resizes, but NOT when all resize
    events are stripped (otherwise the minimizer can fabricate junk by
    deleting cursor-movement bytes between two legitimate prompt prints)."""
    if not _raw_repro(lines):
        return False
    no_r = [l for l in lines if not l.startswith("R")]
    return not _raw_repro(no_r)


def ddmin_lines(lines):
    """Delta-debug style: try removing chunks, halving chunk size."""
    chunk = max(1, len(lines) // 2)
    while chunk >= 1:
        i = 0
        removed_any = False
        while i < len(lines):
            cand = lines[:i] + lines[i + chunk:]
            if cand and reproduces(cand):
                lines = cand
                removed_any = True
            else:
                i += chunk
        if chunk == 1 and not removed_any:
            break
        chunk = chunk // 2 if chunk > 1 else (1 if removed_any else 0)
    return lines


def split_o_chunks(lines):
    """Split big O chunks in half so ddmin can drop finer pieces."""
    out = []
    for ln in lines:
        if ln.startswith("O ") and len(ln) > 2 + 2 * 64:
            hx = ln[2:].strip()
            mid = (len(hx) // 4) * 2
            out.append("O %s\n" % hx[:mid])
            out.append("O %s\n" % hx[mid:])
        else:
            out.append(ln)
    return out


def trim_o_tails(lines):
    """Binary-search shorten each O chunk from both ends."""
    for i in range(len(lines)):
        if not lines[i].startswith("O "):
            continue
        hx = lines[i][2:].strip()
        # trim tail
        lo, hi = 0, len(hx) // 2          # keep [0, 2*k)
        while lo < hi:
            mid = (lo + hi) // 2
            cand = lines[:i] + ["O %s\n" % hx[:2 * mid]] + lines[i + 1:]
            cand = [l for l in cand if l.strip() != "O"]
            if reproduces(cand):
                hi = mid
            else:
                lo = mid + 1
        hx = hx[:2 * lo]
        # trim head
        n = len(hx) // 2
        lo2, hi2 = 0, n                   # drop first lo2 bytes
        while lo2 < hi2:
            mid = (lo2 + hi2 + 1) // 2
            cand = lines[:i] + ["O %s\n" % hx[2 * mid:]] + lines[i + 1:]
            cand = [l for l in cand if l.strip() != "O"]
            if reproduces(cand):
                lo2 = mid
            else:
                hi2 = mid - 1
        hx = hx[2 * lo2:]
        if hx:
            lines[i] = "O %s\n" % hx
        else:
            lines[i] = ""
    return [l for l in lines if l]


def main():
    src, dst = sys.argv[1], sys.argv[2]
    lines = [l for l in open(src) if l.strip()]
    assert reproduces(lines), "input trace does not reproduce"
    print("start: %d events" % len(lines))

    prev = -1
    while prev != len(lines):
        prev = len(lines)
        lines = ddmin_lines(lines)
        print("after ddmin: %d events" % len(lines))
        nlines = split_o_chunks(lines)
        if len(nlines) != len(lines) and reproduces(nlines):
            lines = ddmin_lines(nlines)
            print("after split+ddmin: %d events" % len(lines))
    lines = trim_o_tails(lines)
    lines = ddmin_lines(lines)
    print("after byte-trim: %d events" % len(lines))

    assert reproduces(lines)
    with open(dst, "w") as f:
        f.writelines(lines)
    total = sum((len(l) - 3) // 2 for l in lines if l.startswith("O "))
    print("wrote %s: %d events, %d O-bytes" % (dst, len(lines), total))


if __name__ == "__main__":
    main()
