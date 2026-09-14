#!/usr/bin/env bash
# Drives the game past the title screen and menu with xdotool, then prints a perf digest.
#
#   tools/drive-game.sh [seconds] [tag] [key:drag,...] [extra emulator args...]
#
# Keys are pressed at the given second of the run (the emulator window is matched by name).
# Defaults: k (Square) at 55 s, j (Cross) at 80 s and 100 s, which is the press-to-start,
# continue, and load sequence. Pass "" to skip input entirely.
#
# Example:
#   tools/drive-game.sh 300 ingame "55:k,80:j,100:j" --render-scale auto
#
# Requires xdotool on the host; runs the emulator on XWayland so the X11 input path works.
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_DIR/_Build/linux"
SECONDS_TO_RUN="${1:-300}"
TAG="${2:-drive}"
KEYS="${3:-55:k,80:j,100:j}"
EXTRA_ARGS=("${@:4}")
WINDOW_NAME="${WINDOW_NAME:-SpongeBob}"
LOG="$BUILD_DIR/${TAG}_log.txt"

cd "$REPO_DIR"
SDL_VIDEODRIVER=x11 PRINTF_DIRECTION=File PRINTF_FILE="${TAG}_log.txt" \
	KYTY_FPS_LOG=1 KYTY_XFER_LOG=1 KYTY_READBACK_LOG=1 \
	TIMEOUT="$SECONDS_TO_RUN" ./tools/run-sponge.sh "${EXTRA_ARGS[@]}" >/dev/null 2>&1 &
RUN_PID=$!

if [ -n "$KEYS" ]; then
	start=$(date +%s)
	for entry in ${KEYS//,/ }; do
		target="${entry%%:*}"
		key="${entry##*:}"
		now=$(date +%s)
		delay=$((target - (now - start)))
		if [ "$delay" -gt 0 ]; then sleep "$delay"; fi
		window=$(xdotool search --name "$WINDOW_NAME" 2>/dev/null | head -1 || true)
		if [ -z "$window" ]; then
			echo "drive-game: window '$WINDOW_NAME' not found at ${target}s"
			continue
		fi
		xdotool windowactivate --sync "$window" 2>/dev/null || true
		xdotool key --clearmodifiers "$key"
		echo "drive-game: pressed '$key' at ${target}s"
	done
fi

wait "$RUN_PID" 2>/dev/null || true
python3 "$REPO_DIR/tools/perf-digest.py" "$LOG"
