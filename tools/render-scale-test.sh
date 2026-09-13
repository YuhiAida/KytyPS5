#!/usr/bin/env bash
# Render-scale iteration harness for SpongeBob (PPSA26893).
#
# Purpose: run one render-scale experiment end to end and print a compact report so iteration
# stays cheap: optional build, run with diagnostics, screenshots at fixed times, then summarise
# fps / render-scale decisions / texture transfers / fatal errors / GPU load.
#
# Usage: tools/render-scale-test.sh [options]
#   --width W        window width  (default 1280)
#   --height H       window height (default 720)
#   --scale S        KYTY_RENDER_SCALE override: 1 disables scaling, 0.5 fixes it
#                    (only meaningful on builds that include the render-scale feature)
#   --timeout S      run duration in seconds (default 150)
#   --shots "45 90"  screenshot times in seconds (default "45 90"; empty = none)
#   --interval S     present dump interval in seconds (default: timeout/40, min 2)
#   --tag NAME       override the log tag (default rs_<W>x<H>[_s<scale>])
#   --no-build       skip the container build
#   -h|--help        show this help
#
# Output:
#   _Build/linux/rs_shot_1.png .. rs_shot_N.png   presented frames (written by the emulator)
#   _Build/linux/present_dump/*.ppm               raw dumps behind those screenshots
#   _Build/linux/rs_<tag>_guestlog.txt            guest/emulator log of the run
#   compact report on stdout
# Exit codes: 0 ok, 1 bad usage/input
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_LOG_DIR="$REPO_DIR/_Build/linux"
BUILD=1
WIDTH=1280
HEIGHT=720
SCALE=""
TIMEOUT_RUN=150
SHOTS="45 90"
TAG_OVERRIDE=""
DUMP_INTERVAL_OVERRIDE=""

usage() { sed -n '2,20p' "$0"; }

while [[ $# -gt 0 ]]; do
	case "$1" in
		--width) WIDTH="$2"; shift 2 ;;
		--height) HEIGHT="$2"; shift 2 ;;
		--scale) SCALE="$2"; shift 2 ;;
		--timeout) TIMEOUT_RUN="$2"; shift 2 ;;
		--shots) SHOTS="$2"; shift 2 ;;
		--interval) DUMP_INTERVAL_OVERRIDE="$2"; shift 2 ;;
		--tag) TAG_OVERRIDE="$2"; shift 2 ;;
		--no-build) BUILD=0; shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "unknown option: $1" >&2; usage; exit 1 ;;
	esac
done

cd "$REPO_DIR"

if [[ "$BUILD" == "1" ]]; then
	echo "== build =="
	CONTAINER_RUNNER=podman ./tools/build-quiet.sh 2>&1 | tail -4
fi

for p in $(pgrep -x kyty_emulator); do kill -9 "$p" 2>/dev/null || true; done

TAG="${TAG_OVERRIDE:-rs_${WIDTH}x${HEIGHT}${SCALE:+_s$SCALE}}"
GUESTLOG="$BUILD_LOG_DIR/${TAG}_guestlog.txt"
ERRLOG="$BUILD_LOG_DIR/sponge_stderr.txt"

rm -f "$BUILD_LOG_DIR"/rs_shot_*.png
rm -rf "$BUILD_LOG_DIR/present_dump"
mkdir -p "$BUILD_LOG_DIR/present_dump"
# Screenshots come from the emulator itself (KYTY_PRESENT_DUMP): exact presented pixels, no
# compositor involvement. Desktop capture cannot see the game window on Wayland.
DUMP_INTERVAL=${DUMP_INTERVAL_OVERRIDE:-$((TIMEOUT_RUN / 40))}
[ "$DUMP_INTERVAL" -lt 2 ] && DUMP_INTERVAL=2
DUMP_MAX=$((TIMEOUT_RUN / DUMP_INTERVAL + 4))
( for _ in $(seq 1 $((TIMEOUT_RUN + 40))); do
	nvidia-smi --query-gpu=utilization.gpu,utilization.memory --format=csv,noheader
	sleep 1
done > "$BUILD_LOG_DIR/${TAG}_gpu.txt" ) &
GPU_SAMPLER=$!

setsid env TIMEOUT="$TIMEOUT_RUN" WIDTH="$WIDTH" HEIGHT="$HEIGHT" PRINTF_DIRECTION=File \
	PRINTF_FILE="${TAG}_guestlog.txt" KYTY_PIPELINE_CACHE=1 KYTY_FPS_LOG=1 KYTY_SUB_LOG=1 \
	KYTY_XFER_LOG=1 KYTY_RENDER_SCALE_LOG=1 ${SCALE:+KYTY_RENDER_SCALE="$SCALE"} \
	KYTY_PRESENT_DUMP="$BUILD_LOG_DIR/present_dump" \
	KYTY_PRESENT_DUMP_INTERVAL="$DUMP_INTERVAL" KYTY_PRESENT_DUMP_MAX="$DUMP_MAX" \
	${KYTY_TRACE_LOG:+KYTY_TRACE_LOG="$KYTY_TRACE_LOG"} \
	${KYTY_NO_PM4_DRAIN:+KYTY_NO_PM4_DRAIN="$KYTY_NO_PM4_DRAIN"} \
	${KYTY_OPCODE_LOG:+KYTY_OPCODE_LOG="$KYTY_OPCODE_LOG"} \
	${KYTY_READBACK_LOG:+KYTY_READBACK_LOG="$KYTY_READBACK_LOG"} \
	${KYTY_RENDER_SCALE_ALL:+KYTY_RENDER_SCALE_ALL="$KYTY_RENDER_SCALE_ALL"} \
	./tools/run-sponge.sh > "$BUILD_LOG_DIR/${TAG}_run.txt" 2>&1 < /dev/null &
RUN_PID=$!

wait "$RUN_PID" 2>/dev/null || true
kill "$GPU_SAMPLER" 2>/dev/null || true

# Keep one PNG per requested time: pick the dumped frame closest to that time.
i=0
for t in $SHOTS; do
	i=$((i + 1))
	target_ms=$((t * 1000))
	best=""
	best_delta=""
	for ppm in "$BUILD_LOG_DIR"/present_dump/*.ppm; do
		[ -f "$ppm" ] || continue
		stamp=$(basename "$ppm" | sed -E 's/^([0-9]+)ms_.*/\1/')
		delta=$((stamp > target_ms ? stamp - target_ms : target_ms - stamp))
		if [ -z "$best" ] || [ "$delta" -lt "$best_delta" ]; then
			best="$ppm"
			best_delta="$delta"
		fi
	done
	if [ -n "$best" ]; then
		convert "$best" "$BUILD_LOG_DIR/rs_shot_$i.png" 2>/dev/null ||
			cp "$best" "$BUILD_LOG_DIR/rs_shot_$i.png"
		echo "shot $i (~${t}s) <- $(basename "$best")"
	fi
done

echo "== report: ${TAG} (window ${WIDTH}x${HEIGHT}${SCALE:+, scale=$SCALE}) =="
echo "-- fps --"
grep -a "Present: fps" "$GUESTLOG" | tail -4 || true
echo "-- render scale decisions (first 8) --"
grep -a "RenderScale:" "$GUESTLOG" | head -8 || true
echo "render_scale_lines=$(grep -ac 'RenderScale:' "$GUESTLOG" || true)"
echo "-- xfer --"
grep -a "Xfer:" "$GUESTLOG" | tail -2 || true
echo "-- fatal lines --"
grep -aE "EXIT|fatal|Unhandled|Signal" "$ERRLOG" | tail -5 || true
echo "fallback_memory=$(grep -ac 'fallback memory' "$GUESTLOG" || true)"
echo "-- gpu (late 60s) --"
tail -60 "$BUILD_LOG_DIR/${TAG}_gpu.txt" |
	awk -F, '{gsub(/%/,"",$1); v=$1+0; s+=v; if (v>m) {m=v} n++}
	         END {if (n>0) printf "sm_avg=%.0f sm_max=%d n=%d\n", s/n, m, n; else print "no samples"}'
echo "-- shots (from the emulator present dump) --"
ls "$BUILD_LOG_DIR"/rs_shot_*.png 2>/dev/null || echo none
echo "dumps=$(ls "$BUILD_LOG_DIR"/present_dump/*.ppm 2>/dev/null | wc -l)"
