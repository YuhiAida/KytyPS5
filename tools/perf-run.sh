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

python3 "$REPO_DIR/tools/perf-digest.py" "$LOG"
