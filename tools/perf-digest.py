#!/usr/bin/env python3
"""Compact digest of a run capture produced by tools/perf-run.sh or tools/drive-game.sh.

Usage: tools/perf-digest.py <log> [--tail-seconds N]

Prints fps, present-gap, guest readback-stall, transfer and device-heap summaries from the
opt-in pacing/transfer counters; the raw capture file is never dumped.
"""

import re
import statistics
import sys

FPS = re.compile(r'Present: fps=([\d.]+) maxgap=([\d.]+)')
READBACK = re.compile(r'Readback: count=(\d+) read=(\d+) cpu-write=(\d+) \(bulk=(\d+) small=(\d+)\)'
                      r' wait=([\d.]+) ms/s max=([\d.]+)')
XFER = re.compile(r'Xfer: upload=([\d.]+) MB/s \((\d+) calls\) download=([\d.]+) MB/s')
UPLOAD_SAME = re.compile(r'Xfer: uploadCpu=([\d.]+) ms/s frame_same=(\d+) frame_new=(\d+)')
HEAP = re.compile(r'TextureCache: device heap (\d+) MiB of (\d+) MiB')


def stats(values, fmt):
    if not values:
        return 'n/a'
    return f'min={fmt % min(values)} med={fmt % statistics.median(values)} max={fmt % max(values)}'


def main():
    path = sys.argv[1]
    tail = 0
    if '--tail-seconds' in sys.argv:
        tail = int(sys.argv[sys.argv.index('--tail-seconds') + 1])

    fps, gaps, waits, maxes, xfer, same, heap = [], [], [], [], [], [], []
    try:
        with open(path, 'rb') as handle:
            for raw in handle:
                line = raw.decode('utf-8', 'ignore')
                if (m := FPS.search(line)):
                    fps.append(float(m.group(1)))
                    gaps.append(float(m.group(2)))
                elif (m := READBACK.search(line)):
                    waits.append(float(m.group(6)))
                    maxes.append(float(m.group(7)))
                elif (m := XFER.search(line)):
                    xfer.append((float(m.group(1)), float(m.group(3))))
                elif (m := UPLOAD_SAME.search(line)):
                    same.append((float(m.group(1)), int(m.group(2)), int(m.group(3))))
                elif (m := HEAP.search(line)):
                    heap.append((int(m.group(1)), int(m.group(2))))
    except FileNotFoundError:
        print(f'perf-digest: no capture at {path}')
        return 1

    if tail and len(fps) > tail:
        fps, gaps = fps[-tail:], gaps[-tail:]

    print(f'== perf digest: {path} ==')
    print(f'fps windows : {stats(fps, "%.1f")}')
    if len(fps) >= 10:
        print(f'fps early/steady : {statistics.mean(fps[:10]):.1f} early / '
              f'{statistics.median(fps[len(fps) // 2:]):.1f} median (second half)')
    print(f'present gap : {stats(gaps, "%.0f")} ms')
    worst = sorted(zip(gaps, fps), reverse=True)[:5]
    print('worst windows (gap ms -> fps): ' + ', '.join(f'{g:.0f}->{p:.1f}' for g, p in worst))
    print(f'readback    : windows={len(waits)} wait ms/s {stats(waits, "%.0f")}')
    if maxes:
        print(f'readback max: worst window {max(maxes):.0f} ms, median of per-window max '
              f'{statistics.median(maxes):.0f} ms')
    print(f'xfer        : upload {stats([u for u, _ in xfer], "%.0f")} MB/s  '
          f'download {stats([d for _, d in xfer], "%.0f")} MB/s')
    if same:
        print(f'tex reupload: {statistics.median([c for _, c, _ in same]):.0f} same-content '
              f'/s, {statistics.median([n for _, _, n in same]):.0f} new/s, '
              f'cpu {statistics.median([c for c, _, _ in same]):.0f} ms/s')
    if heap:
        over = sum(1 for used, budget in heap if used > budget)
        print(f'device heap : {len(heap)} samples, {over} over budget; '
              f'last {heap[-1][0]} MiB of {heap[-1][1]} MiB')
    return 0


if __name__ == '__main__':
    sys.exit(main())
