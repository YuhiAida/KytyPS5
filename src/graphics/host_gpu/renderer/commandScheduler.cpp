#include "graphics/host_gpu/renderer/commandScheduler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace Libs::Graphics {

static thread_local CommandScheduler* g_deferred_callback_scheduler = nullptr;

namespace {

void ReportVulkanFatal(const char* what, vk::Result result, uint64_t tick, uint32_t debug_op,
                       uint64_t debug_submit, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                       uint32_t arg3, uint64_t arg4) {
	LOGF("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	     " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	     what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	     debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::printf("%s failed: %s (%d), tick=%" PRIu64 " debug_op=%u debug_submit=%" PRIu64
	            " args=%u,%u,%u,%u,0x%016" PRIx64 "\n",
	            what, vk::to_string(result).c_str(), static_cast<int>(result), tick, debug_op,
	            debug_submit, arg0, arg1, arg2, arg3, arg4);
	std::fflush(stdout);
}

} // namespace

namespace {

struct GpuTimeTopEntry {
	uint64_t ns       = 0;
	uint64_t hash     = 0;
	uint32_t group_x  = 0;
	uint32_t group_y  = 0;
	uint32_t group_z  = 0;
	uint32_t mode     = 0;
	uint32_t indirect = 0;
};

struct GpuTimeStats {
	uint64_t                       batches    = 0;
	uint64_t                       dispatches = 0;
	uint64_t                       span_ns    = 0;
	uint64_t                       busy_ns    = 0;
	uint64_t                       post_ns    = 0;
	uint64_t                       gap_ns     = 0;
	std::array<GpuTimeTopEntry, 3> top {};
	std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();

	void AddTop(const GpuTimeTopEntry& entry) {
		GpuTimeTopEntry candidate = entry;
		for (auto& slot: top) {
			if (candidate.ns > slot.ns) {
				const auto displaced = slot;
				slot                 = candidate;
				candidate            = displaced;
			}
		}
	}
};

GpuTimeStats          g_gpu_time_stats;
std::atomic<uint64_t> g_gpu_time_submits {0};
std::atomic<uint64_t> g_gpu_time_truncated {0};

void GpuTimeMaybeLog() {
	auto&      stats = g_gpu_time_stats;
	const auto now   = std::chrono::steady_clock::now();
	if (now - stats.last_log < std::chrono::seconds(1)) {
		return;
	}
	const auto ms = [](uint64_t ns) { return static_cast<double>(ns) / 1.0e6; };
	LOGF("GpuTime: batches=%llu disp=%llu submits=%llu trunc=%llu span=%.0fms busy=%.0fms "
	     "post=%.0fms gap=%.0fms\n",
	     static_cast<unsigned long long>(stats.batches),
	     static_cast<unsigned long long>(stats.dispatches),
	     static_cast<unsigned long long>(g_gpu_time_submits.exchange(0)),
	     static_cast<unsigned long long>(g_gpu_time_truncated.exchange(0)), ms(stats.span_ns),
	     ms(stats.busy_ns), ms(stats.post_ns), ms(stats.gap_ns));
	for (const auto& entry: stats.top) {
		if (entry.ns == 0) {
			continue;
		}
		LOGF("GpuTimeTop: %.2fms shader=0x%016" PRIx64 " groups=%ux%ux%u mode=0x%08" PRIx32
		     " indirect=%u\n",
		     static_cast<double>(entry.ns) / 1.0e6, entry.hash, entry.group_x, entry.group_y,
		     entry.group_z, entry.mode, entry.indirect);
	}
	stats          = {};
	stats.last_log = now;
}

} // namespace

CommandScheduler::CommandPool::CommandPool(GraphicContext& graphics, MasterSemaphore& master)
    : m_graphics(graphics), m_master(master) {
	EXIT_IF(graphics.queue_family == static_cast<uint32_t>(-1));
	vk::CommandPoolCreateInfo create {};
	create.queueFamilyIndex = graphics.queue_family;
	create.flags            = vk::CommandPoolCreateFlagBits::eTransient |
	                          vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
	const auto result       = graphics.device.createCommandPool(&create, nullptr, &m_pool);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_pool == nullptr);
}

CommandScheduler::CommandPool::~CommandPool() {
	m_graphics.device.destroyCommandPool(m_pool, nullptr);
}

size_t CommandScheduler::CommandPool::Grow() {
	const auto first = m_ticks.size();
	m_ticks.resize(first + GrowStep);
	m_buffers.resize(first + GrowStep);

	vk::CommandBufferAllocateInfo allocate {};
	allocate.commandPool        = m_pool;
	allocate.level              = vk::CommandBufferLevel::ePrimary;
	allocate.commandBufferCount = static_cast<uint32_t>(GrowStep);
	EXIT_IF(m_graphics.device.allocateCommandBuffers(&allocate, m_buffers.data() + first) !=
	        vk::Result::eSuccess);
	return first;
}

vk::CommandBuffer CommandScheduler::CommandPool::Commit() {
	auto       gpu_tick = m_master.KnownGpuTick();
	const auto search   = [this, &gpu_tick](size_t begin, size_t end) -> std::optional<size_t> {
		for (size_t index = begin; index < end; ++index) {
			if (gpu_tick >= m_ticks[index]) {
				m_ticks[index] = m_master.CurrentTick();
				return index;
			}
		}
		return std::nullopt;
	};

	auto found = search(m_hint, m_ticks.size());
	if (!found) {
		m_master.Refresh();
		gpu_tick = m_master.KnownGpuTick();
		found    = search(m_hint, m_ticks.size());
	}
	if (!found) {
		found = search(0, m_hint);
	}
	if (!found) {
		found           = Grow();
		m_ticks[*found] = m_master.CurrentTick();
	}

	m_hint = (*found + 1) % m_ticks.size();
	return m_buffers[*found];
}

bool CommandScheduler::InDeferredOperation() noexcept {
	return g_deferred_callback_scheduler != nullptr;
}

CommandScheduler::CommandScheduler(RenderContext& context, GraphicContext& graphics)
    : m_master(graphics), m_context(context), m_graphics(graphics),
      m_command_pool(graphics, m_master), m_command(*this),
      m_priority_thread([this](std::stop_token stop) { PriorityOperationsThread(stop); }) {
	GpuTimeInitialize();
}

CommandScheduler::~CommandScheduler() {
	Shutdown();
	if (m_gpu_time.pool != nullptr) {
		m_graphics.device.destroyQueryPool(m_gpu_time.pool, nullptr);
		m_gpu_time.pool = nullptr;
	}
}

void CommandScheduler::Shutdown() {
	{
		std::unique_lock lock(m_operation_mutex);
		if (m_operation_state == OperationState::Closed) {
			return;
		}
		if (g_deferred_callback_scheduler == this) {
			EXIT_IF(m_operation_state == OperationState::Open);
			// A priority callback cannot join its own runner, while a normal callback can be
			// executing inside the shutdown owner's final PopPendingOperations. The owning
			// thread will finish shutdown after this callback returns.
			return;
		}
		if (m_operation_state == OperationState::Draining) {
			m_operation_available.wait(
			    lock, [this] { return m_operation_state == OperationState::Closed; });
			return;
		}
		m_operation_state = OperationState::Draining;
	}
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1, "shutdown");
	PopPendingOperations();
	DrainPriorityOperations();
	m_priority_thread.request_stop();
	m_operation_available.notify_all();
	if (m_priority_thread.joinable()) {
		m_priority_thread.join();
	}
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(!m_pending_operations.empty() || !m_priority_operations.empty() ||
		        m_priority_active);
		m_operation_state = OperationState::Closed;
	}
	m_operation_available.notify_all();
}

void CommandScheduler::Begin(HW::Context& registers, HW::UserConfig& user_config,
                             HW::Shader& shaders) {
	{
		std::lock_guard lock(m_operation_mutex);
		EXIT_IF(m_operation_state != OperationState::Open);
	}
	m_command.Bind(registers, user_config, shaders);

	if (m_command.IsInvalid()) {
		BeginNext();
	}
}

void CommandScheduler::BeginRendering(const RenderState& state) {
	Current().BeginRendering(state);
}

void CommandScheduler::EndRendering() {
	if (Active() && !m_command.IsInvalid()) {
		Current().EndRendering();
	}
}

void CommandScheduler::Flush() {
	SubmitInfo submit;
	Flush(submit);
}

void CommandScheduler::Flush(SubmitInfo& submit) {
	Submit(submit);
	BeginNext();
}

void CommandScheduler::FlushAndWait() {
	const auto tick = Submit();
	m_master.Wait(tick, "flush-wait");
	BeginNext();
}

void CommandScheduler::Finish() {
	CheckActive();
	if (!m_command.IsInvalid()) {
		Submit();
	}
	m_master.Wait(CurrentTick() - 1, "finish");
	BeginNext();
	PopPendingOperations();
}

void CommandScheduler::Wait(uint64_t tick) {
	EXIT_IF(tick > CurrentTick());
	if (tick == CurrentTick()) {
		CheckActive();
		// A stream-buffer wrap can wait while a draw is being prepared through a reference to
		// Current(). The wrapper stays stable while its pooled Vulkan buffer is retired. Deferred
		// resources are released only at the next GPU operation boundary.
		const auto submitted_tick = Submit();
		EXIT_IF(submitted_tick != tick);
		m_master.Wait(tick, "wait-current");
		BeginNext();
	} else {
		m_master.Wait(tick, "wait");
	}
}

void CommandScheduler::PopPendingOperations() {
	m_master.Refresh();
	GpuTimeDrainReadbacks();
	for (;;) {
		PendingOperation operation;
		{
			std::lock_guard lock(m_operation_mutex);
			if (m_pending_operations.empty() ||
			    !m_master.IsFree(m_pending_operations.front().tick)) {
				return;
			}
			operation = std::move(m_pending_operations.front());
			m_pending_operations.pop();
		}
		WaitPriorityOperations(operation.tick);
		RunOperation(std::move(operation.callback));
	}
}

void CommandScheduler::DeferOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_pending_operations.push({std::move(operation), CurrentTick()});
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock,
	                           [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

void CommandScheduler::DeferPriorityOperation(Common::UniqueFunction<void>&& operation) {
	CheckActive();
	EXIT_IF(!operation);
	std::unique_lock lock(m_operation_mutex);
	if (m_operation_state == OperationState::Open) {
		m_priority_operations.push({std::move(operation), CurrentTick()});
		lock.unlock();
		m_operation_available.notify_one();
		return;
	}
	if (g_deferred_callback_scheduler == this) {
		lock.unlock();
		operation();
		return;
	}
	m_operation_available.wait(lock,
	                           [this] { return m_operation_state == OperationState::Closed; });
	lock.unlock();
	operation();
}

void CommandScheduler::PriorityOperationsThread(std::stop_token stop) {
	while (!stop.stop_requested()) {
		PendingOperation operation;
		{
			std::unique_lock lock(m_operation_mutex);
			m_operation_available.wait(lock, [this, &stop] {
				return stop.stop_requested() || !m_priority_operations.empty();
			});
			if (stop.stop_requested()) {
				return;
			}
			operation = std::move(m_priority_operations.front());
			m_priority_operations.pop();
			m_priority_active      = true;
			m_priority_active_tick = operation.tick;
		}
		m_master.Wait(operation.tick, "priority");
		if (!stop.stop_requested()) {
			RunOperation(std::move(operation.callback));
		}
		{
			std::lock_guard lock(m_operation_mutex);
			m_priority_active      = false;
			m_priority_active_tick = 0;
		}
		m_operation_available.notify_all();
	}
}

void CommandScheduler::DrainPriorityOperations() {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(
	    lock, [this] { return m_priority_operations.empty() && !m_priority_active; });
}

void CommandScheduler::WaitPriorityOperations(uint64_t tick) {
	EXIT_IF(g_deferred_callback_scheduler == this);
	std::unique_lock lock(m_operation_mutex);
	m_operation_available.wait(lock, [this, tick] {
		const bool active_before_or_at = m_priority_active && m_priority_active_tick <= tick;
		const bool queued_before_or_at =
		    !m_priority_operations.empty() && m_priority_operations.front().tick <= tick;
		return !active_before_or_at && !queued_before_or_at;
	});
}

void CommandScheduler::RunOperation(Common::UniqueFunction<void>&& operation) {
	auto* previous                = g_deferred_callback_scheduler;
	g_deferred_callback_scheduler = this;
	operation();
	g_deferred_callback_scheduler = previous;
}

bool CommandScheduler::IsFree(uint64_t tick) {
	if (m_master.IsFree(tick)) {
		return true;
	}
	m_master.Refresh();
	return m_master.IsFree(tick);
}

void CommandScheduler::CheckActive() const {
	EXIT_IF(!Active());
}

CommandBuffer& CommandScheduler::Current() {
	CheckActive();
	return m_command;
}

CommandBuffer& CommandScheduler::BeginCommand() {
	EXIT_IF(!m_command.IsInvalid());
	m_command.m_buffer = m_command_pool.Commit();
	m_command.Begin();
	GpuTimeBeginBatch();
	return m_command;
}

uint64_t CommandScheduler::Submit(SubmitInfo submit) {
	EXIT_IF(m_command.IsInvalid());
	EXIT_IF(submit.num_wait_semaphores > SubmitInfo::MaxSemaphores ||
	        submit.num_signal_semaphores >= SubmitInfo::MaxSemaphores);

	if (m_gpu_time.batch_span_open) {
		m_command.m_buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe,
		                                  m_gpu_time.pool, m_gpu_time.batch_first + 1u);
		m_gpu_time.batch_span_open = false;
	}
	m_command.End();
	const auto buffer   = m_command.m_buffer;
	auto&      graphics = m_graphics;
	EXIT_IF(graphics.queue == nullptr);

	vk::Result result;
	uint64_t   tick;
	{
		Common::LockGuard lock(graphics.queue_mutex);
		tick = m_master.NextTick();
		submit.AddSignal(m_master.Handle(), tick);

		vk::TimelineSemaphoreSubmitInfo timeline_info {};
		timeline_info.waitSemaphoreValueCount   = submit.num_wait_semaphores;
		timeline_info.pWaitSemaphoreValues      = submit.wait_ticks.data();
		timeline_info.signalSemaphoreValueCount = submit.num_signal_semaphores;
		timeline_info.pSignalSemaphoreValues    = submit.signal_ticks.data();

		vk::SubmitInfo submit_info {};
		submit_info.pNext                = &timeline_info;
		submit_info.waitSemaphoreCount   = submit.num_wait_semaphores;
		submit_info.pWaitSemaphores      = submit.wait_semaphores.data();
		submit_info.pWaitDstStageMask    = submit.wait_stages.data();
		submit_info.commandBufferCount   = 1;
		submit_info.pCommandBuffers      = &buffer;
		submit_info.signalSemaphoreCount = submit.num_signal_semaphores;
		submit_info.pSignalSemaphores    = submit.signal_semaphores.data();

		result = graphics.queue.submit(1, &submit_info, nullptr);
	}

	if (result != vk::Result::eSuccess) {
		ReportVulkanFatal("vkQueueSubmit", result, tick, m_command.m_debug_op,
		                  m_command.m_debug_submit_id, m_command.m_debug_arg0,
		                  m_command.m_debug_arg1, m_command.m_debug_arg2, m_command.m_debug_arg3,
		                  m_command.m_debug_arg4);
	}
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	static uint32_t trace_count = 0;
	if (std::getenv("KYTY_TRACE_LOG") != nullptr && trace_count++ < 512) {
		LOGF("Sched[%p]: submit tick=%llu waits=%u signals=%u", static_cast<const void*>(this),
		     static_cast<unsigned long long>(tick), submit.num_wait_semaphores,
		     submit.num_signal_semaphores);
		for (uint32_t index = 0; index < submit.num_wait_semaphores; index++) {
			LOGF(" w[%u]=%p/%llu", index, static_cast<const void*>(submit.wait_semaphores[index]),
			     static_cast<unsigned long long>(submit.wait_ticks[index]));
		}
		for (uint32_t index = 0; index < submit.num_signal_semaphores; index++) {
			LOGF(" s[%u]=%p/%llu", index, static_cast<const void*>(submit.signal_semaphores[index]),
			     static_cast<unsigned long long>(submit.signal_ticks[index]));
		}
		LOGF("\n");
	}

	m_command.m_buffer = nullptr;
	if (m_gpu_time.pool != nullptr) {
		g_gpu_time_submits.fetch_add(1, std::memory_order_relaxed);
		if (!m_gpu_time.batch_truncated) {
			GpuTimeBatch batch;
			batch.first = m_gpu_time.batch_first;
			batch.slots = m_gpu_time.cursor - m_gpu_time.batch_first;
			batch.meta  = std::move(m_gpu_time.batch_meta);
			if (batch.slots >= 2u) {
				batch.tick = tick;
				m_gpu_time_batches.push_back(std::move(batch));
			}
		} else {
			g_gpu_time_truncated.fetch_add(1, std::memory_order_relaxed);
			m_gpu_time.batch_meta.clear();
		}
	}
	return tick;
}

void CommandScheduler::BeginNext() {
	CheckActive();
	BeginCommand();
}

void CommandScheduler::GpuTimeInitialize() {
	if (std::getenv("KYTY_GPU_TIME_LOG") == nullptr) {
		return;
	}
	if (m_graphics.device == nullptr || m_graphics.physical_device == nullptr) {
		return;
	}
	const auto family   = m_graphics.queue_family;
	const auto families = m_graphics.physical_device.getQueueFamilyProperties();
	if (family >= families.size() || families[family].timestampValidBits == 0) {
		LOGF("GpuTime: timestamp queries unsupported on queue family %u\n", family);
		return;
	}
	const auto period =
	    static_cast<double>(m_graphics.physical_device_properties.limits.timestampPeriod);
	if (period <= 0.0) {
		return;
	}
	// 3 slots per dispatch bracket; 2 per batch span. Wraps are deferred until every pending
	// readback finished so live query data is never overwritten.
	constexpr uint32_t k_capacity = 3u * 65536u + 2u;
	vk::QueryPoolCreateInfo info {};
	info.queryType  = vk::QueryType::eTimestamp;
	info.queryCount = k_capacity;
	const auto result = m_graphics.device.createQueryPool(&info, nullptr, &m_gpu_time.pool);
	if (result != vk::Result::eSuccess || m_gpu_time.pool == nullptr) {
		LOGF("GpuTime: createQueryPool failed (%s)\n", vk::to_string(result).c_str());
		m_gpu_time.pool = nullptr;
		return;
	}
	m_gpu_time.capacity  = k_capacity;
	m_gpu_time.period_ns = period;
	LOGF("GpuTime: enabled period_ns=%.2f capacity=%u\n", period, k_capacity);
}

void CommandScheduler::GpuTimeBeginBatch() {
	if (m_gpu_time.pool == nullptr) {
		return;
	}
	m_gpu_time.batch_meta.clear();
	m_gpu_time.batch_truncated = false;
	m_gpu_time.dispatch_open   = false;
	m_gpu_time.batch_span_open = false;
	if (m_gpu_time.cursor + 2u + 3u > m_gpu_time.capacity) {
		if (m_gpu_time_batches.empty()) {
			m_gpu_time.cursor = 0;
		} else {
			m_gpu_time.batch_truncated = true;
			g_gpu_time_truncated.fetch_add(1, std::memory_order_relaxed);
			return;
		}
	}
	m_gpu_time.batch_first     = m_gpu_time.cursor;
	m_gpu_time.cursor += 2u;
	m_gpu_time.batch_span_open = true;
	m_command.m_buffer.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_gpu_time.pool,
	                                  m_gpu_time.batch_first);
}

void CommandScheduler::GpuTimeDispatchBegin(const CommandBuffer& buffer,
                                            const GpuDispatchMeta& meta) {
	if (m_gpu_time.pool == nullptr || m_gpu_time.batch_truncated) {
		return;
	}
	if (m_gpu_time.cursor + 3u > m_gpu_time.capacity) {
		m_gpu_time.batch_truncated = true;
		g_gpu_time_truncated.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	m_gpu_time.dispatch_slot = m_gpu_time.cursor;
	m_gpu_time.cursor += 3u;
	m_gpu_time.dispatch_open = true;
	buffer.Handle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_gpu_time.pool,
	                               m_gpu_time.dispatch_slot);
	m_gpu_time.batch_meta.push_back(meta);
}

void CommandScheduler::GpuTimeDispatchMid(const CommandBuffer& buffer) {
	if (!m_gpu_time.dispatch_open) {
		return;
	}
	buffer.Handle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_gpu_time.pool,
	                               m_gpu_time.dispatch_slot + 1u);
}

void CommandScheduler::GpuTimeDispatchEnd(const CommandBuffer& buffer) {
	if (!m_gpu_time.dispatch_open) {
		return;
	}
	buffer.Handle().writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, m_gpu_time.pool,
	                               m_gpu_time.dispatch_slot + 2u);
	m_gpu_time.dispatch_open = false;
}

void CommandScheduler::GpuTimeDrainReadbacks() {
	if (m_gpu_time.pool == nullptr || m_gpu_time_batches.empty()) {
		return;
	}
	m_master.Refresh();
	while (!m_gpu_time_batches.empty() && m_master.IsFree(m_gpu_time_batches.front().tick)) {
		GpuTimeBatch ready = std::move(m_gpu_time_batches.front());
		m_gpu_time_batches.erase(m_gpu_time_batches.begin());
		GpuTimeReadBatch(std::move(ready));
	}
}

void CommandScheduler::GpuTimeReadBatch(GpuTimeBatch batch) {
	std::vector<uint64_t> values(batch.slots, UINT64_MAX);
	const auto            result = m_graphics.device.getQueryPoolResults(
	    m_gpu_time.pool, batch.first, batch.slots, values.size() * sizeof(uint64_t), values.data(),
	    sizeof(uint64_t), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
	if (result != vk::Result::eSuccess) {
		static std::atomic<uint32_t> fail_count {0};
		if (fail_count.fetch_add(1, std::memory_order_relaxed) < 8) {
			LOGF("GpuTime: getQueryPoolResults failed (%s) first=%u slots=%u\n",
			     vk::to_string(result).c_str(), batch.first, batch.slots);
		}
		return;
	}
	const double period = m_gpu_time.period_ns;
	uint64_t     span_ns = 0;
	if (values.size() >= 2 && values[0] != UINT64_MAX && values[1] != UINT64_MAX &&
	    values[1] >= values[0]) {
		span_ns = static_cast<uint64_t>(static_cast<double>(values[1] - values[0]) * period);
	}
	uint64_t batch_busy = 0;
	uint64_t batch_post = 0;
	auto&    stats      = g_gpu_time_stats;
	stats.batches++;
	stats.span_ns += span_ns;
	for (size_t index = 0; index < batch.meta.size(); index++) {
		const auto base = 2u + index * 3u;
		if (base + 2u >= values.size()) {
			break;
		}
		const auto t0 = values[base];
		const auto t1 = values[base + 1u];
		const auto t2 = values[base + 2u];
		if (t0 == UINT64_MAX || t1 == UINT64_MAX || t2 == UINT64_MAX || t1 < t0 || t2 < t1) {
			continue;
		}
		const auto busy = static_cast<uint64_t>(static_cast<double>(t1 - t0) * period);
		const auto post = static_cast<uint64_t>(static_cast<double>(t2 - t1) * period);
		batch_busy += busy;
		batch_post += post;
		stats.dispatches++;
		GpuTimeTopEntry entry;
		entry.ns       = busy;
		entry.hash     = batch.meta[index].shader_hash;
		entry.group_x  = batch.meta[index].group_x;
		entry.group_y  = batch.meta[index].group_y;
		entry.group_z  = batch.meta[index].group_z;
		entry.mode     = batch.meta[index].mode;
		entry.indirect = batch.meta[index].indirect;
		stats.AddTop(entry);
	}
	stats.busy_ns += batch_busy;
	stats.post_ns += batch_post;
	if (span_ns > batch_busy + batch_post) {
		stats.gap_ns += span_ns - batch_busy - batch_post;
	}
	GpuTimeMaybeLog();
}

} // namespace Libs::Graphics
