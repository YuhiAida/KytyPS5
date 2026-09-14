# SpongeBob playability investigation

## Iteration 1
- Action: Read the performance handover, inspected `SetPredication`, and adopted the repository bug-hunt workflow.
- Evidence: handover attributes roughly 16 `IT_SET_PREDICATION` waits/s at about 99 ms each; `graphicsRun.cpp` drains unconditionally when `wait_op != 0`.
- Result: candidate root cause is an unnecessary GPU drain when the predicate word is already CPU-current.
## Iteration 2 (take-over session)
- Landed `2defcf8` (predication drain narrowed + readback/VRAM diagnostics). op20 left the PM4
  top-5 profile; fps unchanged because the frame is host-GPU bound.
- Evidence: 100% SM + memory controller at 1905 MHz, ~630 ms/frame, ~6.6 GiB VRAM, ~700 MB/s
  texture uploads; NO_GC / VRAM-reserve / window-size A/B runs all identical (1.3-1.7 fps).
- Readback waits (KYTY_READBACK_LOG) total ~1.1 s/s but are CPU waiting on the busy GPU; the
  op24 `DrawIndirect` CPU read of indirect args is the visible stall site (stack samples).
- Conclusion: CPU side is no longer the gate; render-scale / GPU-side work is the only lever
  with headroom. Next: GPU timing (timestamp queries or RenderDoc) before renderer changes.

## Iteration 3 (plan review of render-scale v2; no code changes)
- Repo state: HEAD `3cf52de` (`fix/spongebob-consolidated`); v1 preserved as `wip/render-scale-v1`
  (based on `2defcf8`, single reusable commit `a7d5ea5`); working tree dirty with docs/tools only.
- Transfers funnel confirmed: `Image::Upload/Download` (image.cpp:220/259) with 10 call sites
  (textureCache.cpp x9, tiler.cpp:412) -> staging-bridge blast radius is small.
- Leftover: `swapchain.cpp:814` still reads `KYTY_RENDER_SCALE_LOG` (inert post-revert).
- Doubt 1 (blocks step ordering): v1 data cannot split the win between texture/upload bandwidth
  and 4K raster of video-out/RTs. Experiment: on `wip/render-scale-v1`, A/B "textures-only" vs
  "video-out-only" scaling, >=180 s runs, screenshot-verified.
- Doubt 2: identity of the 440-740 MB/s uploads is unknown (KYTY_XFER_LOG has counts, no binding
  attribution) - needs attribution before sizing step 3.
- Next: mechanism-split A/B on the v1 branch before building the staging bridge.

## Iteration 4 (mechanism-split experiment on `exp/render-scale-split`)
- Built `exp/render-scale-split` = `wip/render-scale-v1` + `KYTY_RENDER_SCALE_CATS` category
  subset knob (commit `689510a`); host build `./tools/build-quiet.sh` BUILD OK.
- Run C1 (`KYTY_RENDER_SCALE_CATS=rt,tex`, 720p window, 200 s): presents stop at 15032 ms;
  only 16 fps seconds (12.8-41.7, avg 28.4 = light phase); guest abort at the end
  (`Guest abort()` log L311427, diagnostics L310957). No heavy-phase data from this run.
- Aliasing evidence in the same log: flip buffer `0xcfc2000000` created with host=1280x720 and
  its guest-extent upload clamped, while the other flip buffer `0xcfc0000000` stayed native
  ("native surface bound type=2 wanted=0.333 ...") -> one guest address can hold differently
  sized host images per binding type; which one wins is binding-order dependent.
- Assumption to test: abort may be a cold-pipeline-cache artifact ("invalidating
  _PipelineCache/PPSAN.bin" in this first run); retest warm before blaming the config.
- C2 (`rt,vo`) not run - context guard reached. Handoff seed: binary is already built, run
  `flatpak-spawn --host bash -lc 'cd /home/yuhi/Projects/PS5emu/KytyPS5 && export KYTY_RENDER_SCALE_CATS=rt,vo && ./tools/render-scale-test.sh --no-build --timeout 200 --shots "120 180" --tag c2_rtvo'`
  then compare the fps tail and `rs_shot_2.png`; if it also dies early, read the guest lines
  before log L310957 for the abort cause and retest C1 with a warm pipeline cache.

## Iteration 5 (freeze mechanism bisected; branch `exp/render-scale-split`)
- Symptom: every scaled run presents ~14 s, then the guest render thread stops; Unreal's
  watchdog aborts the game at ~138 s (`RenderingThread.cpp:1163`, then `Guest abort()`).
  Trace: GPU timeline frozen (`MasterSemaphore ... waiting tick=5569 current=5568`, never
  completes); GPU idle. Same class as the v1 "queue wedge".
- Category bisect (same binary, 75 s probes, `Present: fps` seconds before stop):
  native 39 alive | rt-only 41 alive | tex+storage 15 FREEZE | rt+tex+skip flips 15 FREEZE |
  tex = sampled textures only 40 alive | sto = storage only 15 FREEZE.
- Conclusion: **scaling `Storage` images kills the GPU queue** (guest-sized compute dispatches /
  storage access cannot target a smaller backing). Sampled textures + render targets are stable
  alone. This was the v1 plan's unaddressed warning ("compute dispatch dimensions cannot be
  scaled safely by extent alone").
- Knobs on the branch: `KYTY_RENDER_SCALE_CATS=rt,tex,sto,vo` and
  `KYTY_RENDER_SCALE_SKIP=0x...,0x...` (addresses kept native in every binding; used to pin
  flip buffers since `FindImage` shares one host image per address and first-binder-wins: a
  flip buffer scaled by one binding type presents a clamped top-left crop - seen live).
- In flight: 300 s runs `m1_rttex_skip` (flip buffers pinned native) and `m2_rttex_all`
  (flip buffers scalable) -> `_Build/linux/m1_rttex_skip_guestlog.txt`,
  `_Build/linux/m2_rttex_all_guestlog.txt`.
- Handoff seed: read those two reports (fps tail, `Sub:`, `Xfer:`, screenshots). If either
  survives 300 s with heavy-phase fps > 2, the texture/RT lever is real and v2 becomes:
  staging bridge (step 1) + sampled textures (step 3); **Storage must never scale while
  dispatch dims are guest-sized**; production flip-buffer guard belongs at
  `VideoOutSetBufferAttribute2`/`SubmitChangeBufferAttribute2` (videoOut.cpp:1234/1422).

### Iteration 5 results (M1/M2, 300 s each)
- `m1_rttex_skip` (flip buffers native): fps tail 1.2/1.7/1.4/1.1; `Sub: slow 749-763 ms
  (translate, gc=0 flush=0)`; uploads 500-622 MB/s; presentation stops ~112 s though the run
  lasted 300 s (nearest dump to the 240 s target is 112182 ms).
- `m2_rttex_all` (flip buffers scalable): fps tail 1.5/1.3/1.7/1.2; `Sub: slow 596-860 ms`;
  uploads 528-637 MB/s; same ~110 s stop (last dump 109932 ms); flip buffer 0xcfc2000000
  scaled with clamped upload (crop) again.
- **Conclusion: scaling render targets + sampled textures gives NO heavy-phase gain**
  (1.1-1.7 fps = the documented native heavy phase) and both runs still die (~110 s, same
  freeze class). The earlier session's "ALL reaches 30 fps" numbers were boot/light-phase
  figures, not the heavy title. Render scale alone is not the heavy-phase lever: the
  600-860 ms/submission CPU-side wait chain persists with much less GPU work.
- Diagnostic knobs committed on `exp/render-scale-split` (storage split + address skip).
- Handoff seed: 1) instrument the ~110 s death in m1/m2 (which submission/tick freezes; is
  the last event a clamped copy? add a clamp kill-switch to A/B); 2) profile the 600-860 ms
  `translate` wait with the GPU not saturated (KYTY_TRACE_LOG + KYTY_READBACK_LOG +
  `tools/stall-sample.sh` on an m1-style config) - that chain is the remaining gate;
  3) hard rules for v2: Storage never scaled while dispatch dims are guest-sized; flip
  buffers native (production guard at `videoOut.cpp:1234`) until transfers can resample.

## Iteration 6 (menu gate identified: IT_DRAW_INDIRECT CPU waits)
- Warm-cache re-run `m1_warm` (stable m1 config, 300 s): fps tail 1.2-1.7, GPU sm_avg=32,
  uploads 535-606 MB/s - the m1/m2 numbers are not a cold-cache artifact (compile-storm
  hypothesis dead). Dumps stop ~108 s again.
- Menu-phase opcode profile (`m1_ops_150s.txt`, last summaries): self times tiny
  (`op15=2ms`, `op10=0ms`, `nop.r18=1ms`), but `in3f` (IT_INDIRECT_BUFFER, inclusive) ~50 s/s
  and `in24` (IT_DRAW_INDIRECT, inclusive) **~1.15-1.20 s/s** - the ~600-750 ms frame is
  dominated by DrawIndirect CPU args-read + queue waits; GPU only ~25-32% busy, so the waits
  are fully exposed. `Sub: slow 572-646 ms (translate=...)` persists; readbacks are light
  now (29-55 ms/s vs 1120 ms/s at native).
- Same site the handover documented (`DrawIndirect -> BufferCache::ReadMemory ->
  CommandScheduler::Wait`); at native it was "mostly hidden behind the GPU", at scaled configs
  it is the exposed gate -> scaling cannot lift the menu fps while this chain dominates.
- Conclusion: the playability lever is **GPU-side indirect draws** (`vkCmdDrawIndexedIndirect`
  with the guest args range bound through the buffer cache) plus cheaper Wait semantics, not
  more render scaling. Render scale stays useful for bandwidth/boot; Storage must stay native;
  flip buffers need per-address consistency.
- Open: the ~110 s death of every scaled run (clamp kill-switch A/B still to do).
- Evidence: `_Build/linux/{m1_warm_guestlog.txt,m1_ops_150s.txt,stall_m1heap_guestlog.txt}`.

## Iteration 7 (GPU-side indirect draw: implementation spec; not yet coded)
Goal: remove the per-frame `IT_DRAW_INDIRECT` CPU args read + queue wait (menu gate, `in24`
~1.15-1.20 s/s at 1.3-1.7 fps) by issuing `vkCmdDrawIndexedIndirect` with the guest args bound
through the buffer cache.
Spec:
1. `CommandProcessor::DrawIndirect` (graphicsRun.cpp:1090): for indexed draws with 16/32-bit
   indices and no debug dump, forward `{m_draw_indirect_args_base_addr + data_offset, 20}`
   instead of the raw `memcpy` (which page-faults into `BufferCache::ReadMemory` ->
   `CommandScheduler::Wait`). Keep the CPU path as fallback (index8 expansion,
   `INDEX_BUFFER_SIZE == 0`, debug dumps).
2. Thread `indirect_args_addr/size` through `CommandProcessor::DrawIndex` (graphicsRun.cpp:1061;
   submits via `m_renderer.GetRenderExecutor().DrawIndex` at 1072) into `DrawIndexArgs`
   (render.h:56).
3. `RenderExecutor::DrawIndex` (renderDraw.cpp:1253): when indirect - skip
   `ResolvePrimitiveRestart`, index8 expansion and the CPU count clamp; bind the index buffer at
   `m_index_base_addr` (firstIndex comes from the args on the GPU) with an upper-bound size of
   `m_index_buffer_size * element_size` (fallback to CPU when 0).
4. `ExecutePreparedDraw` emission (renderDraw.cpp:1084): add an indirect branch emitting
   `vk_buffer.drawIndexedIndirect(args_buffer, offset, 1, sizeof(VkDrawIndexedIndirectCommand))`,
   with `(Buffer*, offset) = BufferCache::ObtainBuffer(args_addr, size, false)`
   (bufferCache.h:52). Counts are unknown -> pass 0 to debug info; audit `m_num_instances`
   consumers in `CommandProcessor` before dropping the parse.
5. Verification: build; run m1 config + `KYTY_OPCODE_LOG=1` for ~150 s; expect `in24` to
   collapse and menu fps well above the 1.3-1.7 baseline with no new freezes. Screenshots:
   `tools/render-scale-test.sh --interval 2 --shots "2 5 10 20 40 60"` (interval option added
   this iteration).
Risks: ordering for GPU-written args (rely on the buffer-cache sync used for index data;
validate); the mesh path (guest indices via BDA) must stay untouched; the `INDEX_BUFFER_SIZE`
clamp semantics are lost for indirect draws (log once).
Phase attribution if needed first: `LogDrawPhase` (debug.cpp:618, gated by
`graphics_debug_dump_enabled()`).

## Iteration 8 (GPU-side indirect draws + dispatches IMPLEMENTED; measured)
- Landed (branch `exp/render-scale-split`): raw guest indirect args are handed to
  `vkCmdDrawIndirect` / `vkCmdDrawIndexedIndirect` / `vkCmdDispatchIndirect` with the args range
  bound through the buffer cache (`ObtainBuffer`), instead of the CPU `memcpy` that page-faults
  into `BufferCache::ReadMemory` + `CommandScheduler::Wait`. Sites: `DrawIndirect` (both
  variants, graphicsRun.cpp), `DrawIndex`/`DrawAuto` (renderDraw.cpp), `CpOpDispatchIndirect`
  (pm4Handlers.cpp) + `DispatchDirect` chain, with fallbacks for thread-dimension remapping,
  8-bit indices, unknown INDEX_BUFFER_SIZE and debug dumps.
- Verified: `translate=0-3 ms` (was 600-900 ms); "partial args" parse warnings 0; `in24`/`in16`
  no longer appear in the PM4 summaries; splash screenshot correct (indirect draws produce
  valid output).
- Result: menu fps still ~1.5-2.5 (native 0.7-1.6 with sm 94-100%; scaled 1.5-2.7 with sm
  ~50%); `KYTY_NO_GC=1` changes nothing (gc_ms was a GPU wait in disguise). Uploads unchanged at
  ~600-680 MB/s in every config. Frame is now GPU-work / upload-bound; the CPU stall chain is
  gone (the fps no longer reacts to it because it was masking, not gating, the tail).
- Watch item: `fallback_memory=5` appeared in the native run (was 0 previously) - check for new
  memory pressure from deeper GPU queues.
- Next levers, in order: (1) attribute the debug draw of the ~600 MB/s uploads (per-address
  upload logging; why ~300+ MB/frame at the menu); (2) GPU-side attribution (timestamp queries
  or RenderDoc) of the remaining frame; (3) flip-buffer scaling with per-address consistency
  (still blocked by the alias/crop issue); (4) re-measure M1/M2-class configs after (1)/(2).
- Commits: `tools/render-scale-test: add --interval ...` + `renderer: GPU-side indirect draws
  and dispatches` on `exp/render-scale-split`.

## Iteration 9 (user feedback: methodology + upload attribution; commits 9258cc2, 2bdb6fa, +attr)
- Methodology correction (user): present dumps are swapchain content only; the emulator WINDOW
  (title bar with fps/frame, bottom shader-count strip from `pipelineCache.cpp:398`) is the
  ground truth. Use `spectacle -b -n -f -o` (host) + window capture; host `/tmp` and sandbox
  `/tmp` are different filesystems (copy via the repo).
- User ground truth on build `9361dca-dirty` (= ind3 indirect fixes): title still 1-2 fps,
  shader-load freeze still present, **title background broken/missing**.
- The "Format ... cannot be used as texture" log lines are device-selection chatter
  (vulkanWindow.cpp:390-460, skipped candidate device) - NOT the background cause.
- Upload attribution (new, KYTY_XFER_LOG): a handful of addresses re-upload 7-13x/s:
  `0x307d250000` ~18 MB x10/s, `0x308f750000` 64 MB x1-3/s, `0x307f250000`/`0x3081250000`/
  `0x3080cf0000` ~32 MB x2-3/s -> 450-650 MB/s sustained; boot already churns 8 MB surfaces
  (1080p frames) at 8-9 calls/s, 250-310 MB/s. Sizes match video frame sizes (8 MB = 1080p
  RGBA, 32 MB = 4K RGBA); uploads are triggered by `IsBufferModified() || IsCpuDirty()` on
  bind (textureCache.cpp:1295-1310).
- Working hypothesis: the title background is video/streamed content; the game feeds frames as
  textures continuously, and either (a) the playback/consumption signaling is broken so frames
  churn and the background never lands, or (b) it is legitimate streaming that our upload path
  cannot sustain. Next: identify the consumer (who samples these addresses; video APIs in use)
  and whether the guest re-writes the data between uploads (instrument the dirty-source).
- Framework note: my metric-only conclusions ("pipeline healthy", "fps unchanged because
  GPU-bound") over-trusted instrumentation vs the user-visible result; always pair with a
  window capture and treat user observations as the acceptance test.

## Iteration 10 (upload stream explained; GPU attribution now required)
- User observation (acceptance test): title animates at 1-2 fps and looks visually fine; boot
  sequence perfect at 30 fps, no artifacts. Title background concern retired.
- Video path confirmed in use: `sceAvPlayer` playing `ghost/Content/Movies/Intro.mp4`; decoder is
  FFmpeg software (`videoDec2Decoder.cpp` -> libavcodec). The moving title background is decoded
  video frames pushed as textures - that is the 8/18/32/64 MB upload churn.
- New counters (`KYTY_XFER_LOG`, committed): `uploadCpu` = 1-14 ms/s (upload path is
  asynchronous, CPU-negligible); consecutive-upload content hash: 302 same vs 1064 changed
  (~78% genuinely new frames) at the title -> the upload stream is a symptom, not the frame
  gate. Downloads also observed (up to 175 MB/s) - watch later.
- Conclusion: the title bottleneck is GPU-side (94-100% SM for ~500 ms/frame at 4K, not
  explainable by raster volume alone). CPU-side and upload-side instrumentation is exhausted;
  the missing instrument is GPU attribution: per-pass timestamp queries or a RenderDoc capture.
- Next: add opt-in timestamp queries around the main pass groups (or schedule a RenderDoc
  capture at the title), then target whatever dominates.

## Iteration 11 (user: intro != menu; menu phase measured; compute-bound found)
- User clarification: the intro (boot movie, 30 fps, fine) and the MAIN MENU (1 fps) are
  different screens; earlier "menu = intro video" reasoning was wrong.
- Menu-phase run (native, 300 s, live window capture + nvidia-smi dmon): SM 97-100%,
  memory controller 2-6%; fps profile: boot 30 -> 1.8 -> 3.9-4.2 fps steady -> last seconds
  back at 30.0 fps with uploads 0 MB/s (the 300-650 MB/s upload churn belongs to the slow
  phases, not the fast one).
- Dispatch workload stats (new KYTY_DISPATCH_LOG, committed): slow phases issue 500-970
  dispatches/s, 1.6-4.3M groups/s, with single dispatches of 129600 (=3840x2160/64) or
  262144 (=4096x4096/64) groups - i.e. full-4K compute passes, ~250-500 per frame at 2 fps.
- Conclusion: the menu is COMPUTE-workload bound (guest-issued PM4 dispatches). Explains SM
  100% + idle memory + fps insensitivity to render-target scaling (dispatch dims are never
  scaled by KYTY_RENDER_SCALE_*). Uploads/stalls are symptoms; raster was never the gate.
- Next: identify what the compute passes do - log compute shader addresses (top-5/s) and
  correlate with `_Shaders` dumps; check for a guest feedback loop (e.g. a job re-dispatched
  every frame because a fence/flag we mis-handle) vs legitimate 4K compute pipeline; decide
  fix (dimension scaling would need dispatch dims and storage targets scaled together - the
  v1 storage-scale OOB freeze is the same coupling).

## Iteration 12 (compute storm narrowed; barrier hypothesis eliminated)
- Per-shader attribution (committed): top compute shaders at the title ~
  `0x1480135100` (~90-100 calls/s), `0x15001f4d00` (~60-75/s), `0x14c00b7c00` (~50-64/s);
  remaining ~400/s spread across many shaders. Shader dumps are content-hashed
  (`new_shader_cs_<hash>.spv`, 18688 dumps / 1039 distinct) - no recompile storm.
- Cross-check: opcode log counts 48 top-level `op15`/s while the dispatch counter counts
  ~600/s -> most dispatches arrive nested inside indirect-buffer containers.
- A/B `KYTY_NO_SHADER_BARRIERS=1` (barriers dropped): fps unchanged (1.2-1.4), same dispatch
  counts -> the global shader barriers are NOT the gate; the frame is bound by compute shader
  execution volume (~3M groups/s, ~1.6 ms GPU per dispatch, ~430 dispatches/frame at 1.4 fps).
- Open question: is a title screen issuing ~430 dispatches/frame the game's real workload
  (UE-style compute pipeline) or an emulation-induced re-dispatch loop? Next: map the top
  shader addresses to their dumps (log the guest address in the dump path), inspect the SPIR-V
  (shader-inspect.sh in the container) for queue-processing/loop structure; correlate the
  dispatch count with the video/streaming cadence.
- User context (important): 3060 Ti >> PS5 GPU, so this volume cannot be legitimate for a
  static title screen; also the emulator renders 4K even at a 720p window (render-scale work
  still unlanded).

## Iteration 13 (dispatch shape: many shaders, small grids, ~1 ms each; user: never recovers)
- User: the menu never recovers while observed (steady state, not background work finishing).
- Per-shader group stats (committed): slow phases ~500-970 dispatches/s distributed across MANY
  shaders (top-3 only ~200/s; the rest are many one-off passes), average dispatch ~3.5K groups
  (small grids), i.e. ~1 ms GPU per dispatch -> ~700 dispatches/frame at ~1 ms = the ~700 ms
  frame. Fast (30 fps) phases issue ~70-100/s. Top shader addresses shift between runs/phases
  (0x1480135100, 0x1480128b00, 0x15001e0c00, 0x14c00bbe00...) - no single looped shader.
- Implication: small compute grids costing ~1 ms each is ~100x slower than a 3060 Ti should
  need -> leading hypothesis is now that our translated compute shaders execute pathologically
  slowly (per-thread emulation cost), on top of the sheer dispatch count.
- Next: map top shader addresses to their SPIR-V dumps (thread the guest address into the dump
  path), disassemble and look for pathologies (scalar loops, LDS/atomic emulation, wave64
  handling); OR add per-dispatch GPU timing. Also still open: why ~700 dispatches/frame for
  this screen (vs ~100 in fast phases) - could be mirror of the same slowness (game re-issues
  until something completes) or a genuinely heavier menu pipeline.

## Iteration 14 (menu frame = serialized chain; pipeline creations continue during it)
- Finer breakdown of the slow phase (rb_base_guestlog.txt, 4-4.7 fps): `Sub: slow 190-306 ms
  (translate=190-306)` per submission, occasional 775 ms; `Readback: count=7-17/s wait=208-800
  ms/s max=201-392 ms` (occasional 583 ms single waits); `gc` waits 205-249 ms on some,
  submissions. So the frame is a chain of ~200 ms serialized waits, not one cause.
- Pipeline creations continue through the run: 168 `vkCreateComputePipelines begin` in 75 s
  (45 in the last 100k log lines; one creation spanned ~55k interleaved log lines).
  GraphicsDebugDump is off in these runs, so no fresh shader dumps; the 18688 dumps on disk are
  from earlier sessions (0 written in the last 3 h).
- `KYTY_SKIP_READBACK=1` A/B: inconclusive due to phase-timing variance between runs (A ended
  at 30 fps, B in a slow phase); needs phase-aligned comparison or an in-run toggle.
- User: Pac-Man runs fine -> not a universal emulator breakage; heavy-compute title behaviour
  still to compare.
- Next (ordered): (1) instrument inside the 200-775 ms translate: per-dispatch time split
  between program-cache hit/miss, pipeline lookup/creation and bindings (is a compute pipeline
  created per frame due to cache-key instability?); (2) phase-aligned readback A/B; (3) shader
  identification (thread the guest address into the compile/dump log) if recompiles show up.

## Iteration 15 (dispatch CPU cost eliminated; GPU execution is the wall)
- Per-phase compute timing (KYTY_COMPUTE_LOG, committed): at the title, 600-960 dispatches/s
  cost only ~25-50 ms/s of CPU total (program 10-15, pipeline 0-1, bindings 13-31, dispatch
  1-3 ms/s). Compute dispatch processing (including pipeline lookups) is NOT the bottleneck.
- Slow phases differ from fast phases by dispatch COUNT per frame: ~700 vs ~3-10 (fast phases
  ~70-100 dispatches/s at 30 fps). The title's ~700 dispatches/frame execute on the GPU at
  ~1 ms each -> ~500-700 ms/frame. The compute storm tracks the title's streaming/video
  background (uploads churn in slow phases, zero in fast ones).
- Remaining attribution requires GPU-side timing (timestamp queries) or shader identification;
  both are new instrumentation. No further cheap CPU-side instruments left.
- Session commits (exp/render-scale-split): 9258cc2 (GPU-side indirect draws/dispatches),
  8c7d384/691b51a (upload attribution+timing), a29ff28/a324a0c (dispatch workload stats),
  055ea10 (per-shader stats + barrier A/B), latest (compute phase timing). Render-scale CATS/
  SKIP knobs and storage-freeze findings documented earlier.

## Iteration 16
- Action: coupling probe - 90 s native run, interleaved KYTY_XFER_LOG + KYTY_DISPATCH_LOG +
  KYTY_FPS_LOG (`_Build/linux/couple_probe_guestlog.txt`).
- Facts: slow phases fps 3.8-4.7 = uploads 548-870 MB/s (156-269 calls/s) + dispatches
  664-974/s (2.3-5.3M groups/s, max=129600). Fast window fps 18.4->30.0 = uploads 0.0 MB/s
  (0 calls) + dispatches 0-306/s. Near end: uploads return (421-451 MB/s) with 102-364
  dispatches/s, then stall (fps 0.2, maxgap 48067 ms).
- Inference: the title's compute storm rides its streaming/media pipeline; the no-streaming
  window runs at full 30 fps. The title is a streaming screen, not a static one.
- Assumption: dispatches process the streamed content - producer/consumer link not yet proven.
- Next: identify the hot shaders (address->hash mapping).

## Iteration 17
- Action: added capped (4096) compile log `ShaderCompile: <label> addr=... hash=...` at the
  ShaderRecompiler::Compile call site (pipelineCache.cpp); 120 s run with dumps
  (`--graphics-debug-dump true`, `shadermap2_guestlog.txt`).
- Facts: guest shader addresses ARE host pointers (shader.cpp:117 reinterpret_cast) ->
  `params.code.data()` == guest VA, so no plumbing needed. Top dispatches mapped (42/40/34
  calls): 0x15001fdd00->fb4280e323c4422b, 0x148013c800->e7a546a6e73304fc,
  0x14c00b7c00->7abf671bffb3cd72 (same content also seen at 0x14c00bbe00 earlier -
  relocatable), 0x17c034c200->ddfbf0410accbcc0. Dumps exist for all: `_Shaders/NNNN_new_shader_
  cs_<hash>.spv` + `original/*.bin` (original AGC shader).
- Inference: address->hash->SPIR-V mapping works and is reusable for any future dispatch probe.
- Next: disassemble and classify.

## Iteration 18
- Action: spirv-dis profiling of 4 hot shaders (container, /usr/bin/spirv-dis).
- Facts: fb42/e7a5/7abf = 330/153/214 lines, 0 loops, 0 images, buffer-only (names
  `BufferResource`, `vsharp`), dominated by OpAccessChain/OpIAdd/OpShiftRightLogical/
  OpBranchConditional -> integer bit-manipulation data shaders. ddfb = 7954 lines, 16 loops,
  1144 Bitcast / 742 IAdd / 628 Select / 381 ShiftRight / 322 BitwiseAnd -> heavy branchy
  bit-level processing. All LocalSize 32x1x1.
- Inference: the title runs a GPU compute media/data pipeline (bitstream-style processing);
  streaming uploads are its input. A full-4K dispatch (129600 groups) in ONE dispatch is
  ~8.3M threads ~ 0.3-1 ms even at good throughput - the storm is plausibly genuine work,
  but per-dispatch GPU cost is still unmeasured.
- Assumption (untested): the 3.8-4.7 fps is caused by dispatch count x per-dispatch GPU cost
  (serialization/codegen), not by one or two pathological shaders. Needs GPU timestamps.
- Next: per-dispatch GPU timestamp queries (the instrument that decides genuine-work vs
  serialization vs codegen).

## Handoff seed (2026-09-13, exp/render-scale-split)
- Bug: SpongeBob title/main menu 1-4 fps (30 fps boot/intro/other screens).
- Established: compute storm (~600-970 dispatches/s, max dispatch 129600 groups = full-4K
  pass; integer/bit-manipulation shaders; 548-870 MB/s streaming uploads) co-occurs with
  streaming; zero-streaming windows run 30 fps. Dispatch CPU processing is negligible
  (Compute: dispatch=1-3 ms/s). Barriers ruled out (NO_SHADER_BARRIERS A/B). Storage
  render-scale freezes the GPU queue (unfixed). Flip-buffer alias crop -> KYTY_RENDER_SCALE_
  SKIP workaround; production fix should pin flip buffers in videoOut.cpp (1234/1422).
- Next instrument (do this first): per-dispatch GPU timestamps - vk::QueryPool(2 slots per
  dispatch), vkCmdWriteTimestamp before/after dispatch in renderCompute.cpp DispatchDirect,
  read back ~2 frames later (eNoWait) and log top-N by GPU time; also count pipeline barriers
  per frame in the recorded stream to test the serialization hypothesis.
- Exact run: `TIMEOUT=90 PRINTF_DIRECTION=File PRINTF_FILE=gpudisp_guestlog.txt
  ../../tools/run-sponge.sh` with KYTY_DISPATCH_LOG=1 + new KYTY_GPU_TIME_LOG=1, then
  `grep -a "GpuDisp:|Sub:" f | tail -40`.
- Useful artifacts: `_Shaders/*.spv` + `_Shaders/original/*.bin` + `_candidates.txt`;
  `tools/shader-inspect.sh` (container) accepts "cs <hash>" lines; compile mapping log
  `ShaderCompile: ... addr=... hash=...` (unconditional, capped 4096 - gate behind a switch
  before production).
- Journal: memory/2026-09-13-spongebob-playability.md

## Iteration 19 - readback ablation (negative)
- KYTY_SKIP_READBACK: still collapses to 1.9-3.6 fps. Readback waits are a symptom of a busy
  GPU, not the cause.

## Iteration 20 - compute ablation (positive)
- KYTY_SKIP_COMPUTE=1: slow phase 4 -> ~10 fps. Skip rate showed the game pushes ~3700
  dispatches/s; baseline only gets ~700/s through (backpressure). => compute execution ~60% of
  frame time; effective ~0.9 ms per (small) dispatch.
- KYTY_COMPUTE_TINY=1 (1x1x1 grids): confounded (game derails on wrong values: 0.4-2 fps), but
  dispatch throughput still capped ~700/s => the cap is not proportional to grid size.

## Iteration 21 - freeze forensics
- Freeze reproduced in harness runs: chk2 (presents stop at 33 s, shader compiles continue to
  75 s end), chk3 (48 s present gap, 0.4 fps, frame at 92.9 s still = autosave notice screen).
- Guest face of it: thread 49 waits EVFILT_VIDEO_OUT vblank events (ident=1) and RECEIVES them
  at ~60-70/s through the whole frozen run (5295 waits, sole waiter, stale_skip=0). Flip count
  frozen at 617 (flipPendingNum=0), vblank counter reaches 4679 (~78 s) => present thread alive
  and ticking; the game itself stops submitting flips (it is loading).
- gdb (launched under gdb, ptrace_scope=1): 52 threads, all parked in libc except ONE guest
  thread executing JIT code; SIGILL trap at that moment is the normal x64-emulator fallback
  (hostException.cpp), not a crash. No deadlock.
- Conclusion: "freeze/black screen/menu never appears" = an extraordinarily slow loading phase
  after the intro/autosave, not an emulator lock-up.

## Iteration 22 - load-phase measurements
- The load does: ~32 MiB texture uploads (~32640 KiB avg, 548-870 MB/s), full-4K compute grids
  (129600 groups), ongoing shader compiles (VS/PS/CS counts climb to the end).
- SKIP_COMPUTE lifted the same phase 4->10 fps => compute execution dominates it.
- Pipeline cache: loads/saves fine (24 MB PPSA26893.bin); per-run recompiler count unchanged
  (516/497) => cache is not the load bottleneck. KYTY_RENDER_SCALE=1 = native (only <1 scales).
- Ruled out for the freeze/load: readback waits, event stealing, stale generations, guest-pause
  branch, present-thread stall, vblank delivery, pipeline cache.

## Next steps (for a fresh session)
1. Instrument the compute storm with GPU timestamps (per-dispatch QueryPool) to split execution
   vs serialization - this decides the fix direction.
2. Identify the 4K-sized work: is it the game's asset resolution or keyed off a display query we
   answer? (VideoOutGetOutputStatus reports resolution=1 for 720p window; capture what the game
   asks/reads at load.)
3. Freeze parity: check whether the 48 s present gaps correlate with the present-dump path only
   (harness) or also occur plain.

## Session commits
- 814ea3d compute phase timing; ffbb70d shader compile addr->hash map; 47b11c7 present-tick /
  vtrig / compute ablation diagnostics.

## Iteration 23 - slow-phase resource profile (pivotal)
- CPU sampling during slow phase (fps 1.7-3.3): 0.17-0.94 cores total. Busiest thread ~99%
  (single guest thread), second is the NVIDIA driver thread [vkrt] up to 84%; everything else
  idle. GPU ~40-44% (harness). => the slow phase is NOT CPU- or GPU-bound: it is
  latency/serialization bound.
- x64 SIGILL trap rate (gdb catchpoint, SIGSEGV/BUS passed through): 1479 traps / ~115 s ~ 13/s.
  The illegal-instruction fallback is NOT the bottleneck. Same 1.3-1.4 fps under gdb as native.
- Frame times quantize in ~100 ms units (short slow ~333 ms ~ 3x100; stalls 666-866 ms ~ 7-9x100),
  matching the repeated "Equeue wait timedout: SonyIOManager (timo = 100000)" retry loop seen
  1500-2400x per run.
- Hypothesis (strong): the guest load/menu logic polls the guest-created "SonyIOManager" e-queue
  for an event our emulation never triggers; progress advances only in 100 ms timeouts => 1-4 fps
  and 40-90 s loads / apparent freezes. The compute storm is downstream of this pacing, not the
  cause.
- Next: log the guest-requested filter/ident when those waits time out (extend the timeout log),
  identify the missing event, find the emulator side that should trigger it (user-event or device
  ioctl path), implement, and verify fps + menu-load time.

## Iteration 24 - timeout idents logged (negative)
- Extended the timeout log with filter/ident. Results: RHIInterruptThread 10,175 timeouts @ ~5 ms
  timo, RHISubmissionThread 7,368 @ ~4 ms, SonyIOManager 1,085 @ 100 ms (99.7% timeouts). All
  filter=0 (= untouched output buffer, the ev[] array is OUTPUT in wait-equeue). 12,918 user
  triggers fired, so the e-queue system works; these are the game's normal polls. Not the cause.

## Iteration 25 - dispatch-path lock stall found AND FIXED (verified)
- Instrumented the untimed dispatch segments: `GpuCall total=674-716 ms/s` for ~590 dispatches/s
  (alternating with 42-45 ms/s seconds); the renderer `Compute: program=` segment mirrored it
  (28 vs 656-699 ms/s). Accounting closes: GpuCall ~= program + bindings + dispatch.
- Root cause: `GetComputeProgram` -> `ProgramCache::Get` performs ShaderRecompiler::TranslateProgram
  + CompileProgram + shader-module creation while the caller holds the global PipelineCache mutex.
  `GetGraphicsPipeline` / `GetComputePipeline` likewise run CreatePipelineInternal (driver
  compile) under it. The loader compiles hundreds of shaders; every dispatch/draw lookup on other
  threads blocked behind a compile.
- Fix (kept, committed): new `m_compile_mutex` serializes compiles (also required: VkPipelineCache
  needs external synchronization); `m_mutex` now only guards maps/state. ProgramCache::Get releases
  the cache mutex around translation and re-acquires for a double-checked commit; pipeline getters
  create outside the lock with a re-check under the compile mutex. Lock order: compile -> cache.
- Verified (`_Build/linux/unlock_guestlog.txt` vs `lock_guestlog.txt`): GpuCall 674 -> 27-40 ms/s,
  program 699 -> 8-9 ms/s, lock/pop/endr ~0, no crash, same slow-phase fps otherwise.

## Iteration 26 - frame limiter: GC drain (located; fix attempted; REVERTED)
- Post-lock-fix slow phase: `Sub: slow 589-645 ms (translate=0/6 gc=579-645)`.
  `BufferCache::RunGarbageCollector` drains the whole GPU queue (`m_scheduler.Wait(CurrentTick())`
  + WaitPriorityOperations) when retiring GPU-dirty buffers.
- Attempt A: defer release until `m_scheduler.IsFree(tick)` with a pending list -> hit
  `EXIT("garbage collection retained GPU ownership")`: with deferral the GPU can legitimately
  re-dirty a range before release.
- Attempt B: re-check + skip instead of EXIT; keep WaitPriorityOperations for the
  publish-before-release ordering -> still blocks on the priority writeback backlog
  (gc=1261 ms observed). Dropping that wait would let guest reads see stale memory (reads are
  GPU-thread serialized and trust the tracker/RangeSet state).
- Decision: REVERTED (git checkout). Proper fix needs completion-tracked writebacks (release only
  buffers whose writeback op has run; no queue drain) - design-level, not a drive-by.
- Note: `KYTY_SKIP_READBACK=1` crashed the guest (SIGSEGV in guest code) on stale readbacks -
  diagnostic only, cannot be used as a fix.

## Iteration 27 - remaining drains (open)
- `Sub: slow (translate=193-1039 ms)` remain in the slow phase. Prime suspect:
  `BufferCache::ReadMemory` drains the queue on every CPU read of GPU-written memory
  (`m_scheduler.Wait(CurrentTick())`; structural - the guest read needs the copy completed).
  Secondary: indirect-buffer command reads from GPU-written memory go through the same path.
- Slow-phase fps unchanged (~1.0-1.4); CPU ~1 core; GPU ~44%.
- Next step options: (a) completion-tracked GC + readback releases (the real fix),
  (b) batch/defer guest-visible readbacks where the guest polls instead of consuming
  immediately, (c) revisit render-scale work once the waits are gone to see what the fps
  ceiling becomes.

## Session commits (cont.)
- 47b11c7 present-tick / vtrig / compute ablations; this round: pipelineCache lock split
  (compile outside m_mutex), GpuCall + extended Compute timing, equeue timeout idents.

## Iteration 28 - GC drain FIXED (completion-tracked); limiter now readback-paced
- Implemented the completion-tracked release: GC downloads carry a `WritebackBatch` ticket
  (atomic counter, incremented at queue time, decremented by the priority writeback after it
  runs). Buffers are released by a later GC pass once the ticket hits zero and no newer GPU
  writes exist. No queue drain, no priority-queue wait in the reclaim path.
- Safety fallback added in `DownloadBufferMemory`: when the byte-range set was already consumed
  by an in-flight download, the tracker-reported bytes are copied straight from the host buffer,
  so CPU reads never observe stale guest memory during the publication window (the host buffer
  still holds the GPU data).
- Verified (`gcfinal_guestlog.txt`): `gc=0` on every `Sub: slow` line (was 580-645 ms per
  collection), zero big-gc stalls, no crashes, clean exit. Commit 8a154ac.
- Slow-phase fps unchanged (~1.0-1.4) - the next layer is quantified:
  `Readback: count=21/s wait=932-1633 ms/s max=921 ms` (pm4_guestlog.txt). The frame time is now
  the sum of readback-synchronized GPU work (~44 ms average per synchronous readback; the guest
  polls GPU results ~21x/s). In-order queue: each readback must wait for the GPU to consume the
  backlog since the last poll, so the remaining cost is the GPU work per poll window - i.e. the
  compute storm itself, no longer emulator-side serialization.
- Next: (a) profile one readback interval's GPU work (dispatch stats within ~44 ms windows);
  (b) with the serialization drains out of the way, revisit render-scale / shader efficiency for
  the 4K-grid integer-shader storm; (c) menu-load time should now be measured end-to-end again
  (the gc stalls used to add ~0.6 s per collection during the load).

## Iteration 29 - user-visible freeze isolated: ONE lazy driver compile
- The user-visible intro→menu freeze = one ~41-43 s stall (`Sub: slow 43203 ms
  (translate=42173, gc=1030)`; RT-scale run).
- `PipeCreate` instrumentation (gated KYTY_COMPUTE_LOG): `PipeCreate: n=5 wait=0
  create=41467 ms/s` + `PipeCreate: driver create 41455 ms` -> the entire stall is ONE
  `vkCreateComputePipelines` call. Zero mutex wait.
- Monster module: hash=0x1929ac47f3eaefa0, 1592 guest instructions -> SPIR-V 153,810 words
  (615 KB) in 8 ms (our emitter is fast). Dump:
  `_Build/linux/_Shaders/0364_new_shader_cs_1929ac47f3eaefa0.spv`.

## Iteration 30 - driver cache does not persist the monster; size is not the cause
- Warm run with driver cache loaded (23,966,294 bytes): pipeline still 41,200 ms -> the giant
  entry never persists (blob sizes fluctuate 22.9-24.7 MB across runs -> likely evicted).
- Sibling big modules in the same run: 0xbd2e37f57ad2d6c0 (103,416 words) and
  0x5191f67261c3b7c4 (158,440 words) compile in **600-686 ms** -> module SIZE is not the
  driver's problem; this one module is qualitatively different.

## Iteration 31 - DISABLE_OPTIMIZATION: no-op (DEAD)
- `KYTY_FAST_PIPE_COMPILE=1` (vk::eDisableOptimization on compute creates): driver create
  **42,951 ms** vs raw 41,455 ms. Driver cost is not its optimization stage.
- 41.5 s is near-constant across raw / disable-opt / spirv-opt variants (41.5 / 43.0 / 41.5 s)
  -> looks like a FIXED driver fallback/timeout path, not proportional compile time.

## Iteration 32 - spirv-opt: viable tooling, wrong fix (DEAD as fix)
- Module is valid (`spirv-val` rc=0). Structure: 1,723 function-local vars, 572 phi, 2,383
  labels; top ops: 5908 Bitcast, 4767 Load, 2843 Select, 2826 Store, 2383 Label, 1723 Variable.
- `spirv-opt -O`: 615,240 -> **1,740,164 bytes** (Loads 4767->1121, Labels 2383->1806,
  Stores unchanged 2826). `-Os` ~same.
- In-engine (`KYTY_OPT_SPV`, threshold 100k words, RegisterPerformancePasses): monster
  153,810 -> **436,600 words** (+3,450 ms), driver still **41,474 ms**. Siblings 103k->85k /
  158k->126k (irrelevant, already fast).
- Conclusion: driver time is independent of module size and of spirv-opt -> construct-specific
  pathology in THIS module.

## Iteration 33 - standalone probe built (for construct bisection)
- `_Build/probe_pipebench.cpp` -> `/tmp/pipebench`: times `vkCreateComputePipelines` for one
  module. Enables shaderImageGatherExtended + shaderStorageImageWriteWithoutFormat, API 1.2
  (module caps: GroupNonUniform(Ballot), ImageGatherExtended, SignedZeroInfNanPreserve,
  StorageImageWriteWithoutFormat; SPIR-V 1.3; LocalSize 32 1 1).
- STATUS: returns -13 immediately — the hardcoded layout (set0: 0=SSBO x10, 3/7/33=sampled
  image, 44=sampler x5, 48=SSBO; PC 128 B) does not exactly match the engine's real
  descriptor types. Fix by logging the exact `AddLayoutBindings` result (shaders.cpp:179 site).
- Once working, module variants can be A/B'd off-line in seconds instead of emulator runs.

## Iteration 34 - fix directions (ranked, none proven yet)
1. Identify the construct: histogram-diff the monster against the dumped siblings
   (0xbd2e37f57ad2d6c0 / 0x5191f67261c3b7c4) via a `--graphics-debug-dump` run, then fix the
   emitter for that pattern. Suspects: 1.5 labels per guest instruction, 1.7k function locals,
   908 GLSL ExtInst, ~6k Bitcast, ~2.8k Select.
2. Async/prewarm pipeline compile on a worker thread during load (the m_compile_mutex split
   makes this feasible); lazy first-use creation makes triggering hard.
3. NOT viable: driver-cache persistence, DISABLE_OPTIMIZATION, spirv-opt -O/-Os.

## Handoff seed (fresh small-context session)
- Bug: SpongeBob intro→menu freeze = ONE 41.5 s `vkCreateComputePipelines`
  (hash 0x1929ac47f3eaefa0), every run (driver cache never persists it).
- Repro: `cd _Build/linux && KYTY_RENDER_SCALE=0.5 KYTY_RENDER_SCALE_CATS=rt
  KYTY_PIPELINE_CACHE=1 KYTY_FPS_LOG=1 KYTY_SUB_LOG=1 KYTY_COMPUTE_LOG=1 TIMEOUT=100
  PRINTF_DIRECTION=File PRINTF_FILE=x.txt ../../tools/run-sponge.sh`
- Observable: `PipeCreate: driver create ~41455 ms` + `Sub: slow ~43 s`. Fixed = create < 2 s.
- Logs this session: _Build/linux/{pipeprobe,fastpipe,optspv}_guestlog.txt.
- Uncommitted code: pipelineCache.cpp (PipeCreate timing KYTY_COMPUTE_LOG; 
  MaybeOptimizeShaderSpirv KYTY_OPT_SPV env-gated), shaders.cpp (KYTY_FAST_PIPE_COMPILE,
  proven no-op, env-gated).
- Instruments: _Build/probe_pipebench.cpp (+ /tmp/pipebench), /tmp/mon_opt.spv.
- Exact next command: dump-run (`--graphics-debug-dump true`, KYTY_PIPELINE_CACHE=0) to capture
  the sibling modules, then compare `spirv-dis | grep -o Op[A-Za-z]* | sort | uniq -c`
  histograms against the monster dump to name the pathological construct.

## Iteration 35 - GPU timestamp attribution instrument (KYTY_GPU_TIME_LOG)
- `CommandScheduler` records BOTTOM_OF_PIPE timestamps: batch start/end (BeginCommand/Submit)
  and a 3-point bracket per compute dispatch (renderCompute.cpp emission). Readback drains in
  `PopPendingOperations` once the submission tick is free; 1 Hz `GpuTime:`/`GpuTimeTop:` lines.
- Pitfall found: TOP_OF_PIPE/BOTTOM_OF_PIPE mixing lets consecutive brackets overlap (sum > wall
  time); BOTTOM_OF_PIPE for both ends keeps the sum serialized (log cadence can still cover >1 s).

## Iteration 36 - the fps gate is ONE shader in dispatcher mode
- Slow phase: `GpuTimeTop` dominated by CS 0x1929ac47f3eaefa0 (`groups=30x17x16`, mode=0x41) with
  ~600-900 ms brackets per frame; every other dispatch <= 2.5 ms (300x gap). GPU busy ~= span
  (96-97 %), gap 65-100 ms/s => execution-bound on this shader, not serialization.
- Same shader is the 41.5 s driver compile: dispatcher fallback -> 1712 Function vars + 87-case
  switch -> pathological for both driver and hardware.

## Iteration 37 - root cause + fix: cross-entry tail duplication
- Captured CFG (`KYTY_CFG_DUMP=1`, `_Build/linux/_Shaders/0000_shader_cs_1929ac47f3eaefa0.cfg.txt`):
  selection 7 (7: execz -> 85/8) has region 8..84; block 14 is entered from 3 (outside) and from 13
  (inside); loops 16..79 / 43..78 sit inside the cloned tail.
- Implemented bounded cross-entry duplication in `ShaderCFG.cpp`
  (`DuplicateOneExternalSelectionRegion`, recovered from the parked cb6ead5 commit): clone region
  members the header does not dominate, redirect header-owned edges into clones, join header-owned
  exits through a private synthetic merge; applied one region per attempt AFTER selection routing in
  `Structurize`; loop-closure guard (back edges wholly inside or outside the clone set);
  post-transform dominance verification; budget 8x (32..1024).
- Tests: `shader_cfg_tests` and `shader_recompiler_compute_tests` pass. Also fixed stale test
  expectations (download utility 32 -> 128 MiB, from e6eff87).

## Iteration 38 - verified: freeze gone, fps 1.4 -> 30
- `CFG dispatcher fallback` = 0 (was 1/run, always this hash); `PipeCreate: driver create` = 0
  (no create >= 50 ms anywhere); monster absent from GpuTimeTop (top now <= 2.5 ms).
- fps: 1.1-1.5 -> 3.5/4.5/5.3 then 30.0 sustained windows (16 samples at 30.0) in a 180 s run.

## Iteration 39 - NEW crash after the fix (backtrace captured)
- Run B0 (180 s, dumps on) ends with `Unhandled host exception ... access=1 address=0x0`;
  gdb capture (script `_Build/gdb_crash2.gdb`, guest SIGSEGV passed through) gives the stack:
  `std::fill` <- `BufferCache::FillBuffer` <- `CommandProcessor::DmaData` <- `CpOpDmaData` <- PM4.
- FillBuffer's fast path writes the guest VA directly (`std::fill((uint32_t*)vaddr, ...)`,
  bufferCache.cpp ~552); the page is unmapped, and `RenderContext::HandleFault` declines because
  the range is not in `m_mapped_ranges` (registered by `MapGpuRange` from the kernel memory APIs).
- Facts: fill target 0x3080650000 (guest), first observed at flip ~632, reproducible under gdb;
  everything before it runs at 30 fps.
- Next instruments: log MapGpuRange/UnmapGpuRange (addr+size+API, capped) and the HandleFault
  rejection detail; rerun to see whether the range was never registered or was unmapped (VRAM
  reclaim commits a046563/0c1ca79 are the suspects to rule out).
- Note: the completion-tracked GC release (8a154ac) is currently REVERTED (80696e6, no journal
  rationale); GC stalls are back in `Sub: slow` (0.7-1.3 s) - re-evaluate after the crash.

## Handoff seed (2026-09-13 late session)
- Landed on `fix/spongebob-playability` (single branch off the exp tip): GPU-time instrument,
  gated CFG dump, pipe layout log, structurizer duplication fix, test expectation fix.
- Remaining blocker: the 0x3080650000 DMA-fill crash (Iteration 39). Repro: `TIMEOUT=180
  PRINTF_DIRECTION=File PRINTF_FILE=B.txt KYTY_PIPELINE_CACHE=1 KYTY_FPS_LOG=1 ./tools/run-sponge.sh`
  -> crashes ~60-120 s in. gdb: `gdb -batch -x _Build/gdb_crash2.gdb --args ./kyty_emulator <args>`
  (passes guest SIGILL/SIGSEGV, stops on host faults).
- Frozen evidence: `_Build/linux/B0_struct.txt` (pre-crash), 
  `_Build/linux/_Shaders/0000_shader_cs_1929ac47f3eaefa0.cfg.txt`, `/tmp/gdb_out6.txt` (backtrace).


## Iteration 40 - the "DMA fill" crash was the structurizer, not memory
- The `access=1 address=0x0` crash decodes (code bytes at pc) to
  `mov rbx,[rcx+rax+0x60]` inside `ComputePostDominators` -> `graph.blocks[successors[i]]`
  with a successor id of 0xFFFFFFFF: post-dominator analysis indexed out of bounds.
- Source: `RebuildPredecessors` filters invalid ids for `predecessors` only, and
  `ApplyBlockOrder`/`RemapId` turn a lost block into UINT32_MAX, so the garbage survives in
  `successors`. Root cause: the ported duplication reorder dropped the ORIGINAL clone-source
  blocks (`if (clones.contains(i)) continue;` - verified against the parked original cb6ead5,
  which used `MoveBlockBefore` and never dropped a block); external predecessors then
  referenced dropped ids.
- Fixes: keep the originals (only the appended clone slots move), and make
  `RebuildPredecessors` drop unresolvable successor ids. Add `KYTY_CFG_NO_DUP=1` as an A/B knob.
- Verified: the same 180 s run no longer aborts, reaches VS 201 / PS 304 / CS 240 (was
  VS 89 / PS 105 / CS 167).

## Iteration 41 - second crash: GC download bigger than the ring
- Next blocker: `BufferCache: download of 219941376 bytes (vaddr=0x...3076fa0000
  size=293273600, copies=92) exceeds 128 MiB download staging buffer capacity`.
- The GC drained whole cached buffers; the 512 KiB window used by the CPU-read path is the
  existing pattern, so `RunGarbageCollector` now windows at half the download ring.
- Verified: clean 180 s run (rc=124 = timeout, no EXIT), `fallback_memory` unchanged.

## Iteration 42 - render scale default made opt-in (visual regression)
- User-visible regression: menu/title background missing, content in a top-left third.
  `KYTY_RENDER_SCALE_LOG` shows `guest=3840x2160 host=1280x720`, `upload copy exceeds scaled
  backing ... clamping`, `native surface bound (wanted=0.333)`: the window-size default in
  `WantedRenderScale` (from an earlier exp-commit a7d5ea5) scales the 4K targets to 720p while
  transfers still clamp, so the frame is cropped. Not caused by the structurizer work.
- Fix: default stays 1.0; `KYTY_RENDER_SCALE=<0..1>` remains the opt-in. v2 design requirement
  stands: scaled transfers must resample before the window-size default returns.
- Verified: 1280x720 window now renders the full background again (title + menu captures).

## Iteration 43 - menu-phase profile: 100 GPU drains/s
- Menu is ~8-10 fps at 1280x720 with correct visuals. `wait_flip_done` (nop.r06) is ~0 ms in
  this phase; PM4 handlers sum to ~150 ms/s; GPU util ~69%.
- `KYTY_READBACK_LOG` (now also prints the hot ranges) shows ~100-134 synchronous downloads/s,
  150-190 ms/s of waiting, 10-47 ms each, spread over distinct 16-64 KiB windows.
- Diagnostic `KYTY_SKIP_READBACK=1` (stale data, expected to break correctness) jumps to
  27-57 fps before the guest crashes on stale values: the guest's reads of GPU-written memory
  are forcing full GPU drains (serialisation). Next lever: make GPU writes CPU-visible without
  a drain (eager write-back / GPU-side indirection), not more raster scaling.

## Iteration 44 - render-scale attempt (reverted) and what it proved
- Implemented guest-extent staging + blit resampling for Image::Upload/Download (with
  Image::Transit barriers and a deferred staging delete) plus WantedRenderScale guards.
- Result: with `KYTY_RENDER_SCALE=0.5` the GPU timeline hangs on the first 4K -> 1080p
  resample upload (`MasterSemaphore waiting tick=952 current=951 who=finish`; one present
  dump at 0 ms, then nothing). Ruled out: barrier stage masks, blit filter mode, staging
  lifetime (leaking it still hangs). With resampling disabled the same experiment presents
  fine (dumps=20).
- Prize measured while it was gated off: scale=0.5 with the old clamped path dropped GPU
  utilisation from ~70-77 % to ~22-24 % - the scaling lever is real, the transfer plumbing
  is what is missing. Retry with Vulkan validation enabled before re-landing.
- Kept from the attempt: WantedRenderScale now requires a blittable single-sample colour
  format and keeps depth, compressed, multisampled and video-out surfaces native (v2 policy).

## Iteration 45 - resampling retried: the blit hung on an out-of-bounds rectangle
- The hang was not a barrier/lifetime issue: `ScaledBlitRegion` gave the two sides of the
  blit backwards at both call sites, so an upload recorded source 1920x1080 (the host region)
  on the 3840x2160 staging and destination 3840x2160 (the guest region) on the 1920x1080
  backing. A destination region larger than the image is an out-of-bounds blit: undefined
  behaviour, and the GPU timeline stalls (`MasterSemaphore ... waiting tick=N current=N-1
  who=finish`), which in turn deadlocks the threads that call UnmapMemory/SendCommandSync.
- Proof: `KYTY_RENDER_SCALE_LOG` blit line printed the swapped regions; a temporary
  `KYTY_RENDER_SCALE_NO_BLIT=1` gate kept the staging copy and dropped the blit, and the same
  run presented 48-52 dumps instead of 1.
- Fix (e6adccc): the helper takes explicit source side, clamps both rectangles to their
  extents (the scale factor can round one texel past the larger side) and drops empty regions;
  both call sites corrected. Verified at scale 0.5: 52 dumps in 110 s and a correct title
  screen; the blit now reads `3840x2160[0,0 3840x2160] -> 1920x1080[0,0 1920x1080]`.
- Steady-state A/B (180 s runs, last 12 s): ~8.4 fps at scale 1.0 vs ~11.1 fps at scale 0.5
  (+30 %) at the same ~63 % GPU load. The scene is hitch-bound (recurring 100-350 ms maxgaps,
  shaders still compiling), so the scale stays opt-in; correctness first by default.

## Iteration 46 - window-resolution rendering becomes the launcher default; compressed textures
proven unscalable on this GPU
- Validated `docs/handoff-resolution-fps.md` against the code: all mechanics confirmed, plus three
  corrections - the blit gate tested *either* blit bit; scaled host extents are pixel-rounded where
  compressed images need 4x4 block alignment; and "VideoOut must force native extent" is a code
  fact whose consequence is unproven (for forcing it native would present stale guest memory, so it
  was audited and deliberately left alone).
- New `tools/probe-bc-blit.sh` (bare Vulkan instance, runs on the host): the RTX 3060 Ti reports
  BC1..BC7 `optimalTilingFeatures = 0x1d401` = SAMPLED|BLIT_SRC|LINEAR|TRANSFER_SRC|TRANSFER_DST,
  i.e. **no BLIT_DST** (R8G8B8A8 control = 0x1dd83, has it). Both resample directions need the
  compressed format as the blit *destination*, so the handoff's "BC formats ARE blittable" is
  refuted here: removing the IsBlock guard would record an invalid blit, not a slow one. The driver
  does accept BC images at extents that are not a multiple of 4 (the VU is not enforced).
- Changes: `WantedRenderScale` now requires both blit bits (compressed stays native because the
  hardware says so, not because of a hardcoded guard), default categories are `rt,tex`, "auto"
  reads the live window extent from the graphic context (config as fallback), `--render-scale`
  accepts 'native'/'off' and rejects bad values instead of silently using 1.0, and the launcher has
  a "Render scale" combo (100 / 75 / 50 / Auto) that defaults to Auto for new and existing configs.
- Verified: Vulkan validation layers ON with `--render-scale auto` for 90 s -> 0 VUID, 0 validation
  errors, run reaches the timeout (no abort). Menu scenario 150 s at auto: ~10-12 fps,
  `fallback_memory=0`, GPU ~60 %; native in the same session: ~3.5-7.9 fps with 9 fallback-memory
  allocations.
- Screenshots: the 45 s frame is correct; the 90 s frame shows the known menu-background flap, and
  a pre-change capture (`docs/screenshots/small_rs_shot_2.jpg`) carries the identical artifact, so
  texture scaling did not introduce it. Present dumps stopping mid-run is the dump's
  `m_pending_dump` guard after a stall - observed in native runs too.
- Tests: `shader_recompiler_compute_tests` passes; `shader_cfg_tests` fails on a DS-lane decode
  case in a binary built before this session (untouched area). ctest reports every test "Not Run"
  (container-relative paths) - run the binaries from `_Build/linux-ci/` directly.

## Iteration 47 - the frame is PM4 translation, not raster and not readback (measured)
- New instrument: `src/graphics/gpuPhaseStats.h` + `tools/phase-digest.py`. The KYTY_FPS_LOG line
  now prints a per-second CPU breakdown: `pm4(proc/gc/flush) readback flushwait waitcur waitother
  finish`, all wall times around existing blocking calls, off unless the fps log is on.
- Steady menu phase at render-scale auto (~10.5 fps, ~95 ms per frame):
  `pm4=916 ms/s` of which `proc=900` (the `CommandProcessor::Process` translation loop),
  `readback=114 ms/s` over 120 events/s, `waitcur=119 ms/s` over 120 events/s, `finish=10 ms/s`,
  gc/flush ~15 ms/s. The guest GPU thread is ~92 % busy inside the interpreter: the frame is
  CPU-bound on PM4 translation. That is consistent with the idle GPU (~55 % util) and explains why
  more raster scaling stopped paying.
- Consequence: the readback/drain serialisation is **not** the wall (~11 ms/frame), and the
  "27-57 fps when readbacks are skipped" number from iteration 43 belongs to a lighter phase.
- `KYTY_OPCODE_LOG` on the same phase: 4.0 MB/s of commands; top-level op3f (IT_INDIRECT_BUFFER)
  dominates and the nested buckets (`in3f` ~29.8 s/s summed over all nesting levels) show the cost
  lives inside indirect-buffer streams. Nested accumulation adds every level, so it cannot be
  ranked directly, and the profiler records no nested *counts* - per-opcode self cost is still
  unknown.
- Per-packet work in `ProcessPm4` is unconditional: `std::getenv("KYTY_NO_PM4_DRAIN")`,
  `GraphicsRunDebugDumpEnabled()`, and two `steady_clock::now()` calls feeding the opcode profiler.
  Cheap first candidates, share unmeasured.

## Iteration 48 - the interpreter cost is the draw path, and the draw path is state resolution
- Cached those per-packet lookups and gated the profiler clock reads: **no measurable change**
  (pm4 895 -> 905 ms/s at the same game time, fps unchanged). Kept as hygiene; it was not the win.
- `KYTY_OPCODE_LOG` now reports handler *self* time (it was inclusive, so the indirect-buffer
  recursion multiplied it by the nesting depth). Steady phase, ~10.8 fps, pm4=882-905 ms/s:
  `op35 IT_DRAW_INDEX_OFFSET_2` 357 ms/s (~9k draws/s, 39 us each), `op15 IT_DISPATCH_DIRECT` 163,
  `op25 IT_DRAW_INDEX_INDIRECT` 119, `op2d IT_DRAW_INDEX_AUTO` 119, `op3f IT_INDIRECT_BUFFER` 55,
  `op16 IT_DISPATCH_INDIRECT` 40. Draws and dispatches are ~90% of the interpreter.
- Draws split (per draw, ~41 us total): `shad` (program resolution) ~14 us, `bind`
  (PrepareGraphicsBindings) ~15-23 us, `rt` (AcquireRenderTargets) ~4 us, everything else
  (index setup, vertex buffers, pipeline lookup, descriptor commit, `vkCmdDrawIndexed`) ~4 us
  combined. `uc_check`/`hw_check`/topology: 0.
- Inside `bind`: rimg (FindTexture) 54, find (FindBuffer) 40, img (ResolveTexture) 35,
  rbuf (NativeStorageBuffer + uploads) 27, samplers 0 (already cached).
- Inside `shad`: PrepareProgram 10 (VS) / 0 (PS), key build 0, hash-map find 20,
  **MaterializeResources 141 ms/s** - the snapshot and specialization are rebuilt per stage per
  draw from the immutable plan, with vector copies of plan data and guest-memory reads.

## Iteration 49 - how much of that work could be skipped (measured)
- Repeat rate against the previous draw: programs 74-75%, image bindings 76-77%, buffer bindings
  26-30%, whole draw state identical 26-30% (9.5-10k draws/s).
- So: an all-or-nothing prepared-state cache would skip only ~27%; per-part memoisation of program
  resolution (~95 ms/s, 75% hit) and image resolution (~68 ms/s, 76% hit) is where the money is,
  while buffer caching does not pay (addresses move).
- Path to 30 fps, honestly: the per-draw cost is ~41 us at ~900 draws/frame. Halving it gives
  ~16 fps; 30 fps needs the per-draw state pipeline to change shape (memoised snapshots handed out
  by pointer/shared ownership, batching, or GPU-side state), not another local optimisation.
