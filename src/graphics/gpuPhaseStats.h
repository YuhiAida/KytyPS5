#ifndef GRAPHICS_GPU_PHASE_STATS_H_
#define GRAPHICS_GPU_PHASE_STATS_H_

#include <atomic>
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
