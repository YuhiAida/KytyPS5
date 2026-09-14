# tools/

Repeatable repo automation. **Check here before writing a one-off shell loop.**
Canonical source for `tools/` conventions - agent customizations link here.

## Inventory

| Script | Purpose |
|---|---|
| `digest-log.sh` | Compress a huge log into a small digest. **Never `cat` a log.** |
| `build-quiet.sh` | Build via `build-linux-ci.sh`; print only the result + errors. |
| `test-quiet.sh` | Run `ctest`; print only the failing tests. |
| `build-linux-ci.sh` | Containerised known-good build (podman, clang 18 + LTO). |
| `run-dq.sh` | Reproduce Dragon Quest VII (`PPSA17942`). |
| `run-dq-dump.sh` | DQ run with command-buffer dumps enabled. |
| `run-sponge.sh` | Reproduce SpongeBob (`PPSA26893`) with quiet logging by default. Set `PRINTF_DIRECTION=File` to capture guest/LOGF output in `_Build/linux/sponge_guestlog.txt`. Run from the host (not the Flatpak sandbox). |
| `render-scale-test.sh` | One-command render-scale iteration: optional build, run, compact report (fps, scale decisions, xfer, fatals, GPU load) and `rs_shot_N.png` screenshots taken by the emulator itself (`KYTY_PRESENT_DUMP`, exact presented frames — desktop capture cannot see the game window on Wayland). || `probe-bc-blit.sh` | Host-GPU Vulkan capability report for block-compressed formats (SAMPLED / BLIT_SRC / BLIT_DST / LINEAR and extent alignment). Decides whether compressed textures may be resampled at all: without BLIT_DST they must stay native. || `trace-tail.sh` | Tail only the interesting interleaved lines (scheduler submits, swapchain acquire/present/recover, master-semaphore waits, fps) of a `KYTY_TRACE_LOG` run. |
| `stall-sample.sh` | One-shot stall diagnosis: traced run + per-thread stack sampling at stall time; prints resolved top frames and trace tail. |
| `ea-evidence.sh` | One line per eye-adaptation (histogram) dump: sampled-image fingerprints, resolved SSBO ranges, output value. `--hash 0x...` filters to one shader, `--summary` counts acid/good values + transitions. |
| `shader-set.sh` | Identify which shaders a scene feature uses: `snapshot` the `_Shaders` dump folder for one run, then `diff` two runs that differ only in whether the feature is on screen. |
| `bisect-build.sh` | Bisect a build regression. |

## Rules

- **Extract on second use.** If the same non-trivial command sequence is run
  twice - in a session or across an autonomous run - write `tools/<name>.sh`
  and call that from then on. Do not wrap trivial one-liners.
- **Filtered output only.** Script output becomes agent context. Keep it small,
  cap the line width, and prefer counts over dumps.
- **Prefer the quiet wrappers.** `build-quiet.sh` and `test-quiet.sh` exist
  because raw build/test output is one of the largest token sinks in this repo.

## Script conventions

Model new scripts on `digest-log.sh`:

- `#!/usr/bin/env bash` and `set -euo pipefail`
- support `-h` / `--help`
- meaningful exit codes: `0` ok, `1` bad input, `2` bad usage
- header comment stating purpose, usage and options
- `chmod +x`, and no new external dependencies
- cap output width (`cut -c1-200`)

## Examples

```bash
tools/digest-log.sh _Build/linux/dragonquestlogs.txt --top 25 --tail 40
tools/digest-log.sh _Build/linux/neptunia_logs.txt --pattern 'audio|Atrac9'
./tools/run-dq.sh 2>&1 | tools/digest-log.sh -     # digest a live run
./tools/run-dq-dump.sh 2>&1 | tools/ea-evidence.sh --summary -   # night-scene acid-wash verdict
tools/build-quiet.sh                               # full correct build, filtered
tools/test-quiet.sh -R shader                      # only tests matching "shader"
```
