#!/usr/bin/env python3
"""Fuzz 2D resize schedules against the reflow replayer.

Generates random (cols,rows,delay) schedules from a seeded RNG, captures a
real-zsh PTY trace for each (via pty_trace2d.run_trace), replays it through
test_st_hunt, and keeps any trace whose replay reports joined>0 or frags>3.
"""

import os
import random
import re
import subprocess
import sys

sys.path.insert(0, "/tmp/st-reflow-tests")
import pty_trace2d as pt

OUTDIR = "/tmp/st-reflow-tests"
HUNT = os.path.join(OUTDIR, "test_st_hunt")


def gen_schedule(seed):
    rng = random.Random(seed)
    style = rng.choice(["walk", "bounce", "corner", "mixed"])
    n = rng.randint(40, 200)
    cmin, cmax = rng.choice([(2, 12), (2, 40), (3, 20), (2, 80)])
    rmin, rmax = rng.choice([(2, 10), (3, 24), (2, 24), (4, 12)])
    delays = rng.choice([
        [0.0], [0.005], [0.02], [0.05],
        [0.0, 0.05], [0.005, 0.1], [0.0, 0.0, 0.0, 0.15],
        [0.001, 0.01, 0.03],
    ])
    steps = []
    c, r = 80, 24
    # initial shrink toward the box
    while c > cmax or r > rmax:
        c = max(cmax, c - rng.randint(2, 8))
        r = max(rmax, r - rng.randint(1, 3))
        steps.append((c, r, rng.choice(delays)))
    for i in range(n):
        if style == "walk":
            c = pt.clamp(c + rng.randint(-6, 6), cmin, cmax)
            r = pt.clamp(r + rng.randint(-4, 4), rmin, rmax)
        elif style == "bounce":
            c = rng.choice([cmin, cmax, rng.randint(cmin, cmax)])
            r = rng.choice([rmin, rmax, rng.randint(rmin, rmax)])
        elif style == "corner":
            c = rng.randint(cmin, min(cmax, cmin + 8))
            r = rng.randint(rmin, min(rmax, rmin + 8))
        else:  # mixed: occasional jump to big, mostly tiny
            if rng.random() < 0.15:
                c, r = rng.randint(40, 80), rng.randint(12, 24)
            else:
                c = rng.randint(cmin, cmax)
                r = rng.randint(rmin, rmax)
        steps.append((c, r, rng.choice(delays)))
    steps.append((80, 24, 0.2))
    desc = "style=%s n=%d cols=[%d,%d] rows=[%d,%d] delays=%s" % (
        style, n, cmin, cmax, rmin, rmax, delays)
    return steps, desc


def replay(path):
    out = subprocess.run(
        [HUNT, path], capture_output=True, text=True,
        env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"),
    ).stdout
    m = re.search(r"joined: (\d+)\s+prompt frags: (\d+).*junk: (\d+)", out)
    if not m:
        return None, out
    return tuple(map(int, m.groups())), out


def main():
    seeds = range(int(sys.argv[1]), int(sys.argv[2])) if len(sys.argv) > 2 \
        else range(0, 40)
    shell = sys.argv[3] if len(sys.argv) > 3 else "zsh"
    hits = []
    for seed in seeds:
        steps, desc = gen_schedule(seed)
        label = "fuzz%04d" % seed
        path, events = pt.run_trace(shell, pt.SHELLS[shell], label, steps)
        res, out = replay(path)
        if res is None:
            print("seed %d: replay parse failure: %s" % (seed, out))
            continue
        joined, frags, junk = res
        tag = "  <<< REPRO" if (joined > 0 or frags > 3) else ""
        print("seed %4d: joined=%d frags=%d junk=%d  (%s)%s"
              % (seed, joined, frags, junk, desc, tag))
        if joined > 0 or frags > 3:
            keep = os.path.join(OUTDIR, "trace_repro_fuzz%04d.txt" % seed)
            os.rename(path, keep)
            hits.append((seed, keep, res, desc))
        else:
            os.unlink(path)
    print("\n%d reproducers" % len(hits))
    for seed, keep, res, desc in hits:
        print("  %s  joined=%d frags=%d junk=%d  %s" % (keep, *res, desc))


if __name__ == "__main__":
    main()
