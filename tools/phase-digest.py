#!/usr/bin/env python3
"""Summarise the KYTY_FPS_LOG phase breakdown (``Present: cpu ...`` lines).

The fps logger prints, once per second alongside the fps line, how much wall time the frame
spent in each blocking bucket. This turns those lines into a small table so the question
"where does the frame go?" has an answer without dumping the log.

Usage:
  tools/phase-digest.py <log> [--tail N]

  --tail N   only use the last N one-second windows (default: all of them)
  -h/--help

Output: one line per bucket with mean / median / max ms/s and the mean event count per second,
plus the mean fps over the same windows. Exit codes: 0 ok, 1 missing input, 2 bad usage.
"""

import argparse
import pathlib
import re
import statistics
import sys

BUCKETS = ("pm4", "proc", "gc", "flush", "readback", "flushwait", "waitcur", "waitother",
           "finish", "draw", "pre", "check", "ix", "state", "shad", "exec", "prep", "commit",
           "emit", "bind", "vtx", "rt", "pipe")
MS_RE = re.compile(r"([a-z0-9]+)=([0-9.]+)")
COUNT_RE = re.compile(r"([a-z0-9]+)=[0-9.]+\\(n=([0-9]+)\\)")
FPS_RE = re.compile(r"Present: fps=([0-9.]+)")


def main() -> int:
    parser = argparse.ArgumentParser(add_help=True, description=__doc__)
    parser.add_argument("log")
    parser.add_argument("--tail", type=int, default=0)
    parser.add_argument("--range", dest="window_range", default="",
                        help="only use windows A:B (1-based, inclusive; same game time across runs)")
    args = parser.parse_args()

    path = pathlib.Path(args.log)
    if not path.is_file():
        print(f"phase-digest: no such log: {path}", file=sys.stderr)
        return 1

    buckets: dict[str, list[float]] = {name: [] for name in BUCKETS}
    counts: dict[str, list[float]] = {name: [] for name in BUCKETS}
    fps: list[float] = []

    with path.open("r", errors="ignore") as handle:
        for line in handle:
            if line.startswith(("Present: cpu ", "Present: draw=", "Present: exec=",
                                "Present: prep=")):
                for name, value in MS_RE.findall(line):
                    if name in buckets:
                        buckets[name].append(float(value))
                for name, value in COUNT_RE.findall(line):
                    if name in counts:
                        counts[name].append(float(value))
            elif line.startswith("Present: fps="):
                match = FPS_RE.search(line)
                if match:
                    fps.append(float(match.group(1)))

    windows = len(fps)
    if windows == 0:
        print("phase-digest: no 'Present: fps=' lines (run with KYTY_FPS_LOG=1)", file=sys.stderr)
        return 1

    tail = args.tail if args.tail > 0 else windows
    tail = min(tail, windows)

    if args.window_range:
        start_text, _, end_text = args.window_range.partition(":")
        start = max(1, int(start_text) if start_text else 1)
        end = min(windows, int(end_text) if end_text else windows)
    elif args.tail > 0:
        start, end = max(1, windows - tail + 1), windows
    else:
        start, end = 1, windows
    indices = list(range(start - 1, end))

    def select(values: list[float]) -> list[float]:
        return [values[i] for i in indices if i < len(values)]

    print(f"phase digest: {path}  windows={windows} using {start}-{end}")
    print(f"fps (same windows): mean={statistics.mean(select(fps)):.1f} "
          f"median={statistics.median(select(fps)):.1f}")
    for name in BUCKETS:
        values = select(buckets[name])
        if not values:
            continue
        counts_values = select(counts[name])
        count_note = f" events/s={statistics.mean(counts_values):.0f}" if counts_values else ""
        print(f"  {name:9s} mean={statistics.mean(values):7.1f} "
              f"median={statistics.median(values):7.1f} max={max(values):7.1f} ms/s{count_note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
