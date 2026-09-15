# Plan: per-draw state pipeline (the path to 30 fps) — new branch, not this one

Status: 2026-09-15, not started. Deliberately a separate branch/PR: it changes ownership of
per-draw state, so it needs its own correctness pass. Measurements behind every number below are in
`memory/2026-09-13-spongebob-playability.md` iterations 47-49 and in the tooling described at the
bottom.

## Why

Menu scenario, 1280x720 window, render-scale `auto` (2026-09-15):

- ~11 fps, ~92 ms per frame.
- `pm4` (the PM4 interpreter) ~890 ms/s, i.e. the guest GPU thread is ~90 % busy there.
- Draws are ~400 ms/s of that, at ~9.5-10k draws/s (~900 per frame, ~41 us each).
- Per draw: program resolution ~14 us (`MaterializeResources` alone is 141 ms/s), bindings ~15-23 us
  (`FindTexture` 54, `FindBuffer` 40, `ResolveTexture` 35, storage-buffer/upload 27, samplers 0),
  render targets ~4 us, and the actual `vkCmdDrawIndexed` recording ~1 %.
- Validation (`uc_check`/`hw_check`) and the index-source setup are free.

So ~90 % of a draw is derived state, ~1 % is the draw. Local optimisations of this path top out
around 15-18 fps (the GPU's own work is 65-93 ms/frame). 30 fps needs the work to not happen.

## What to change (ordered by measured value)

1. **Hand out materialised state instead of rebuilding it.** `ProgramCache::Get` rebuilds the
   `ResourceSnapshot` and the `ResourceSpecialization` on every cache hit
   (`MaterializeResources`, 141 ms/s, twice per draw) and `PrepareGraphicsBindings` re-resolves every
   binding. Cache the materialised `(snapshot, specialization, handle)` and the `PreparedBindings`
   in the cache entry, invalidate on guest state change, and give callers a stable pointer
   (generation-tagged) instead of a moved copy.
   - Measured headroom: program resolution repeats 75 % of the time, image bindings 76 %; that is
     ~163 ms/s -> ~+20 % (~13 fps) on its own.
   - The ownership change is the risk: today `input_info.stage.resources` is a value the draw owns
     through commit. Pointers need a lifetime that survives map rehashing and eviction.
2. **Cut the number of resolutions per frame**: coalesce draws that share state, or record repeated
   state once into secondary command buffers.
3. **Move state resolution to the GPU**: the code already hands indirect draw args to the GPU
   (`vkCmdDrawIndexedIndirect`); bindings could follow via push descriptors / BDA so no CPU pass is
   needed per draw.
4. Only after the above: revisit the workload itself (~900 draws/frame is the guest's choice).

## Non-goals / already ruled out

- Raster/fill: render-scale already cut it; the GPU idles at ~55 %.
- Readback/drain serialisation (~110 ms/s) and the flush-and-wait path (~115 ms/s): real but ~10x
  smaller than the interpreter, and the "27-57 fps when readbacks are skipped" figure belongs to a
  lighter phase (loading), not the menu.
- Compressed textures cannot be resampled on this GPU (no BC `BLIT_DST`; see `tools/probe-bc-blit.sh`).

## How to measure it

- Run: `tools/drive-game.sh` (menu/gameplay, xdotool) or `tools/render-scale-test.sh` (menu, 60-120 s).
- Breakdown: `KYTY_FPS_LOG=1` prints per-second phase lines; summarise with
  `tools/phase-digest.py <log> --range A:B` (same game-time windows across builds).
- Per-opcode: `KYTY_OPCODE_LOG=1`, which reports handler *self* time.
- Repeat rates (what a cache could skip): the `Present: repeat ...` line in the same log.
