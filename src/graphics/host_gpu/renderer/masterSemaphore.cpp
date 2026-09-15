#include "graphics/host_gpu/renderer/masterSemaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/graphicContext.h"

namespace Libs::Graphics {

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	if (m_semaphore != nullptr) {
		m_graphics.device.destroySemaphore(m_semaphore, nullptr);
	}
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick, const char* who) {
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	for (;;) {
		const auto result = m_graphics.device.waitSemaphores(&wait_info, 3'000'000'000ull);
		if (result == vk::Result::eSuccess) {
			break;
		}
		if (result == vk::Result::eTimeout) {
			static uint32_t trace_count = 0;
			if (trace_count++ < 32) {
				uint64_t counter = 0;
				m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
				LOGF("MasterSemaphore[%p]: waiting tick=%llu current=%llu who=%s",
				     static_cast<const void*>(this), static_cast<unsigned long long>(tick),
				     static_cast<unsigned long long>(counter), who);
				const auto gpu_tick = KnownGpuTick();
				LOGF(" known=%llu\n", static_cast<unsigned long long>(gpu_tick));
			}
			continue;
		}
		EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	}
	Refresh();}

} // namespace Libs::Graphics
