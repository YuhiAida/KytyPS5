#!/usr/bin/env bash
# Runs the game with the opt-in pacing/transfer logs and prints a compact digest.
#
#   tools/perf-run.sh [seconds] [tag]
#
# Play as usual while it runs. On exit it summarises fps, present gaps, guest readback
# stalls and transfer volumes from _Build/linux/<tag>_log.txt; the raw log stays on disk
# and is never dumped to the terminal.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_DIR/_Build/linux"
SECONDS_TO_RUN="${1:-180}"
TAG="${2:-perf}"
LOG="$BUILD_DIR/${TAG}_log.txt"

cd "$REPO_DIR"
PRINTF_DIRECTION=File PRINTF_FILE="${TAG}_log.txt" \
	KYTY_FPS_LOG=1 KYTY_XFER_LOG=1 KYTY_READBACK_LOG=1 \
	TIMEOUT="$SECONDS_TO_RUN" ./tools/run-sponge.sh >/dev/null 2>&1 || true

python3 - "$LOG" <<'EOF'
import re, statistics, sys

path = sys.argv[1]
fps, gaps, waits, xfer = [], [], [], []
try:
	with open(path, 'rb') as handle:
		for raw in handle:
			line = raw.decode('utf-8', 'ignore')
			if (m := re.search(r'Present: fps=([\d.]+) maxgap=([\d.]+)', line)):
				fps.append(float(m.group(1)))
				gaps.append(float(m.group(2)))
			elif (m := re.search(r'Readback: count=\d+ read=\d+ cpu-write=\d+ \(bulk=\d+ small=\d+\)'
			                     r' wait=([\d.]+) ms/s max=([\d.]+)', line)):
				waits.append((float(m.group(1)), float(m.group(2))))
			elif (m := re.search(r'Xfer: upload=([\d.]+) MB/s .* download=([\d.]+) MB/s', line)):
				xfer.append((float(m.group(1)), float(m.group(2))))
except FileNotFoundError:
	print(f'perf-run: no log at {path}')
	sys.exit(1)

def stats(values, fmt):
	if not values:
		return 'n/a'
	return f'min={fmt % min(values)} med={fmt % statistics.median(values)} max={fmt % max(values)}'

print(f'== perf digest: {path} ==')
print(f'fps windows : {stats(fps, "%.1f")}')
if len(fps) >= 10:
	print(f'fps early/steady : {statistics.mean(fps[:10]):.1f} early / '
	      f'{statistics.median(fps[len(fps) // 2:]):.1f} median (second half)')
print(f'present gap : {stats(gaps, "%.0f")} ms')
worst = sorted(zip(gaps, fps), reverse=True)[:5]
print('worst windows (gap ms -> fps): ' + ', '.join(f'{g:.0f}->{p:.1f}' for g, p in worst))
print(f'readback    : windows={len(waits)} wait ms/s {stats([w for w, _ in waits], "%.0f")}')
if waits:
	mx = [m for _, m in waits]
	print(f'readback max: worst window {max(mx):.0f} ms/s, median of per-window max {statistics.median(mx):.0f} ms')
print(f'xfer        : upload {stats([u for u, _ in xfer], "%.1f")} MB/s  '
      f'download {stats([d for _, d in xfer], "%.1f")} MB/s')
EOF
