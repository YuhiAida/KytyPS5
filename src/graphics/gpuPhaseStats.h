#ifndef GRAPHICS_GPU_PHASE_STATS_H_
#define GRAPHICS_GPU_PHASE_STATS_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

// Per-second breakdown of where the frame's CPU time goes, printed alongside the KYTY_FPS_LOG
// line. The point is attribution: the fps line alone cannot tell "the frame costs CPU in the
// command processor" from "the frame blocks on the GPU", and those need opposite fixes. Every
// bucket below wraps an existing blocking call, so the numbers are wall times (summed over all
// threads that hit the bucket - the caller decides what that means).
//
// Off unless KYTY_FPS_LOG is set: the accumulators are two relaxed atomics per recorded call and
// nothing reads them otherwise.
namespace GpuPhaseStats {

enum class Phase : uint32_t {
	Submit,         // guest GPU submission processing (PM4 translate + gc + flush)
	Pm4Process,     //   .. the PM4 translation loop (CommandProcessor::Process)
	Pm4Gc,          //   .. the renderer garbage collector called between packets
	Pm4Flush,       //   .. CommandProcessor::BufferFlush after packets that made progress
	Readback,       // waiting for the GPU inside BufferCache::ReadMemory
	FlushWait,      // CommandScheduler::FlushAndWait
	GpuWaitCurrent, // CommandScheduler::Wait for the current tick (flushes, then waits)
	GpuWaitOther,   // CommandScheduler::Wait for an older tick
	GpuWaitFinish,  // CommandScheduler::Finish, called on the guest GPU thread once per frame
	DrawTotal,      // RenderExecutor::DrawIndex, whole call
	DrawPre,        //   .. prologue: pending operations, lock, early-out checks
	DrawCheck,      //   .. uc_check/hw_check/topology
	PrepIndex,      //   .. index source, primitive restart, 8-bit expansion
	PrepState,      //   .. PrepareDrawRenderState (render targets, pipeline state)
	PrepShaders,    //   .. RefreshShaders + LogDrawStateIfNeeded
	ExecPrepare,    //   .. PrepareGraphicsBindings, vertex/index buffers, RTs, pipeline lookup
	BindPrep,       //   ..   .. PrepareGraphicsBindings (descriptor sources)
	VtxPrep,        //   ..   .. AcquireVertexBuffers + PrepareIndexBuffer
	RtPrep,         //   ..   .. AcquireRenderTargets
	PipePrep,       //   ..   .. PipelineCache::GetGraphicsPipeline
	PbImages,       //   .. PrepareBindings: ResolveTexture + BindImage per bound image
	PbSamplers,     //   .. PrepareBindings: NativeSampler per sampler + shader data dwords
	FbFind,         //   .. FindBuffers: BufferCache::FindBuffer per bound buffer
	RbBuffers,      //   .. RebindBuffers: NativeStorageBuffer + NativeUpload
	RbImages,       //   .. RebindImages: TextureCache::FindTexture per bound image
	ProgsVs,        //   .. GetGraphicsPrograms: PrepareProgram for the vertex stage
	ProgsPs,        //   .. GetGraphicsPrograms: PrepareProgram for the pixel stage
	ProgsLookup,    //   .. GetGraphicsPrograms: the program-cache lookups
	ProgKey,        //   ..   .. BuildStageStaticKey (the per-draw lookup key)
	ProgFind,       //   ..   .. the hash-map find
	ProgMatl,       //   ..   .. MaterializeResources on a hit
	ExecCommit,     //   .. CommitVertexBuffers/CommitBindings/CommitIndexBuffer, dynamic params
	ExecEmit,       //   .. BeginRendering, bindPipeline, the vkCmdDraw* itself
	Count,
};

inline constexpr uint32_t kPhaseCount = static_cast<uint32_t>(Phase::Count);

inline bool Enabled() {
	static const bool enabled = std::getenv("KYTY_FPS_LOG") != nullptr;
	return enabled;
}

inline std::atomic<uint64_t> g_micros[kPhaseCount];
inline std::atomic<uint64_t> g_counts[kPhaseCount];

inline void Add(Phase phase, double ms) {
	if (!Enabled()) {
		return;
	}
	const auto index = static_cast<uint32_t>(phase);
	g_micros[index].fetch_add(static_cast<uint64_t>(ms * 1000.0), std::memory_order_relaxed);
	g_counts[index].fetch_add(1, std::memory_order_relaxed);
}

// For regions that cannot be wrapped in a scope (their locals outlive the region): take a start
// point with Begin() and close it with AddSince(). Both are free while the breakdown is off.
[[nodiscard]] inline std::chrono::steady_clock::time_point Begin() {
	return Enabled() ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point {};
}

inline void AddSince(Phase phase, const std::chrono::steady_clock::time_point& begin) {
	if (!Enabled()) {
		return;
	}
	Add(phase, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
	                .count());
}

// Times a region and records it on destruction, so early returns inside the region are measured
// too. The enabled flag is sampled once, at construction.
class Scope {
public:
	explicit Scope(Phase phase)
	    : m_phase(phase), m_enabled(Enabled()),
	      m_begin(m_enabled ? std::chrono::steady_clock::now()
	                        : std::chrono::steady_clock::time_point {}) {}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;
	~Scope() {
		if (m_enabled) {
			Add(m_phase, std::chrono::duration<double, std::milli>(
			                 std::chrono::steady_clock::now() - m_begin)
			                 .count());
		}
	}

private:
	Phase                                 m_phase;
	bool                                  m_enabled;
	std::chrono::steady_clock::time_point m_begin;
};

struct Snapshot {
	double   ms[kPhaseCount] {};
	uint64_t count[kPhaseCount] {};

	[[nodiscard]] double Ms(Phase phase) const { return ms[static_cast<uint32_t>(phase)]; }
	[[nodiscard]] uint64_t Count(Phase phase) const { return count[static_cast<uint32_t>(phase)]; }
};

// Reads the window accumulated since the last call and resets it.
inline Snapshot Take() {
	Snapshot snapshot;
	for (uint32_t i = 0; i < kPhaseCount; i++) {
		snapshot.ms[i] =
		    static_cast<double>(g_micros[i].exchange(0, std::memory_order_relaxed)) / 1000.0;
		snapshot.count[i] = g_counts[i].exchange(0, std::memory_order_relaxed);
	}
	return snapshot;
}

} // namespace GpuPhaseStats

#endif // GRAPHICS_GPU_PHASE_STATS_H_
