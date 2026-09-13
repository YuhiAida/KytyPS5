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

