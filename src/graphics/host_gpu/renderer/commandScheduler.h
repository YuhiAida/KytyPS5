#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "graphics/host_gpu/renderer/render.h"

#include <atomic>
#include <condition_variable>
#include <mutex>

#include <queue>

#include <thread>
#include <vector>

namespace Libs::Graphics {

class CommandScheduler {
public:
	CommandScheduler(RenderContext& context, GraphicContext& graphics);
	~CommandScheduler();
	KYTY_CLASS_NO_COPY(CommandScheduler);

	void           Begin(HW::Context& registers, HW::UserConfig& user_config, HW::Shader& shaders);
	void           BeginRendering(const RenderState& state);
	void           EndRendering();
	void           Flush();
	void           Flush(SubmitInfo& submit);
	void           FlushAndWait();
	void           Finish();
	CommandBuffer& BeginCommand();
	uint64_t       Submit(SubmitInfo submit = {});
	// Deferred callbacks can observe an externally owned drain, but cannot initiate shutdown:
	// the priority runner cannot join itself.
	void                      Shutdown();
	void                      Wait(uint64_t tick);
	void                      PopPendingOperations();
	void                      DrainPriorityOperations();
	void                      WaitPriorityOperations(uint64_t tick);
	void                      DeferOperation(Common::UniqueFunction<void>&& operation);
	void                      DeferPriorityOperation(Common::UniqueFunction<void>&& operation);
	// Opt-in GPU-side attribution (KYTY_GPU_TIME_LOG=1): timestamp queries around every compute
	// dispatch and every submission batch, read back without stalling the queue, to separate
	// dispatch execution from serialization inside a batch and idle gaps between submits.
	struct GpuDispatchMeta {
		uint64_t shader_hash = 0;
		uint32_t group_x     = 0;
		uint32_t group_y     = 0;
		uint32_t group_z     = 0;
		uint32_t mode        = 0;
		uint32_t indirect    = 0;
	};
	[[nodiscard]] bool GpuTimeActive() const noexcept { return m_gpu_time.pool != nullptr; }
	void GpuTimeDispatchBegin(const CommandBuffer& buffer, const GpuDispatchMeta& meta);
	void GpuTimeDispatchMid(const CommandBuffer& buffer);
	void GpuTimeDispatchEnd(const CommandBuffer& buffer);
	[[nodiscard]] static bool InDeferredOperation() noexcept;

	[[nodiscard]] bool Active() const noexcept { return m_command.m_registers != nullptr; }
	void                           CheckActive() const;
	CommandBuffer&                 Current();
	[[nodiscard]] uint64_t         CurrentTick() const noexcept { return m_master.CurrentTick(); }
	[[nodiscard]] bool             IsFree(uint64_t tick);
	[[nodiscard]] MasterSemaphore& GetMasterSemaphore() noexcept { return m_master; }
	[[nodiscard]] RenderContext&   Context() const noexcept { return m_context; }
	[[nodiscard]] GraphicContext&  Graphics() const noexcept { return m_graphics; }

private:
	class CommandPool {
	public:
		CommandPool(GraphicContext& graphics, MasterSemaphore& master);
		~CommandPool();
		KYTY_CLASS_NO_COPY(CommandPool);

		vk::CommandBuffer Commit();

	private:
		static constexpr size_t GrowStep = 4;

		size_t Grow();

		GraphicContext&                m_graphics;
		MasterSemaphore&               m_master;
		vk::CommandPool                m_pool = nullptr;
		std::vector<vk::CommandBuffer> m_buffers;
		std::vector<uint64_t>          m_ticks;
		size_t                         m_hint = 0;
	};

	enum class OperationState { Open, Draining, Closed };

	struct PendingOperation {
		Common::UniqueFunction<void> callback;
		uint64_t                     tick = 0;
	};

	struct GpuTimeState {
		vk::QueryPool                pool            = nullptr;
		uint32_t                     capacity        = 0;
		uint32_t                     cursor          = 0;
		double                       period_ns       = 0.0;
		uint32_t                     batch_first     = 0;
		bool                         batch_span_open = false;
		bool                         batch_truncated = false;
		bool                         dispatch_open   = false;
		uint32_t                     dispatch_slot   = 0;
		std::vector<GpuDispatchMeta> batch_meta;
	};

	struct GpuTimeBatch {
		uint32_t                     first = 0;
		uint32_t                     slots = 0;
		uint64_t                     tick  = 0;
		std::vector<GpuDispatchMeta> meta;
	};

	void BeginNext();
	void PriorityOperationsThread(std::stop_token stop);
	void RunOperation(Common::UniqueFunction<void>&& operation);
	void GpuTimeInitialize();
	void GpuTimeBeginBatch();
	void GpuTimeDrainReadbacks();
	void GpuTimeReadBatch(GpuTimeBatch batch);

	MasterSemaphore              m_master;
	RenderContext&               m_context;
	GraphicContext&              m_graphics;
	CommandPool                  m_command_pool;
	CommandBuffer                m_command;
	std::queue<PendingOperation> m_pending_operations;
	std::queue<PendingOperation> m_priority_operations;
	std::mutex                   m_operation_mutex;
	std::condition_variable      m_operation_available;
	std::jthread                 m_priority_thread;
	bool                         m_priority_active      = false;
	uint64_t                     m_priority_active_tick = 0;
	OperationState               m_operation_state      = OperationState::Open;
	GpuTimeState                 m_gpu_time;
	std::vector<GpuTimeBatch>    m_gpu_time_batches;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_COMMANDSCHEDULER_H_
