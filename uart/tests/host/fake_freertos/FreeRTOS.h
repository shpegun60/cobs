/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Recording stand-in for the two FreeRTOS headers uart/FreeRtosWake.h
 * includes. It models only what that adapter uses — the task-notification
 * FromISR call, the yield request, the blocking take and the tick
 * conversion — and records every call so a host test can assert on them.
 * Nothing here is a scheduler.
 */
#ifndef FAKE_FREERTOS_H_
#define FAKE_FREERTOS_H_

#include <cstdint>

using BaseType_t = long;
using UBaseType_t = unsigned long;
using TickType_t = uint32_t;
struct tskTaskControlBlock;
using TaskHandle_t = tskTaskControlBlock*;

#define pdFALSE ((BaseType_t)0)
#define pdTRUE ((BaseType_t)1)
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

namespace fake_freertos {

struct Model {
	TaskHandle_t last_notified = nullptr;
	unsigned notifications_from_isr = 0;
	unsigned yields_requested = 0;      // portYIELD_FROM_ISR(pdTRUE)
	unsigned yields_not_requested = 0;  // portYIELD_FROM_ISR(pdFALSE)
	BaseType_t next_higher_priority_woken = pdFALSE; // what the "kernel" reports
	unsigned pending_count = 0;         // notification value of the one task
	TickType_t last_take_timeout = 0;
	BaseType_t last_take_clear = 0;
};

inline Model& model() noexcept
{
	static Model m;
	return m;
}

inline void reset() noexcept { model() = Model{}; }

} // namespace fake_freertos

inline void portYIELD_FROM_ISR(const BaseType_t woken) noexcept
{
	if (woken != pdFALSE) {
		++fake_freertos::model().yields_requested;
	} else {
		++fake_freertos::model().yields_not_requested;
	}
}

#endif /* FAKE_FREERTOS_H_ */
