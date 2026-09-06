/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */
#ifndef FAKE_FREERTOS_TASK_H_
#define FAKE_FREERTOS_TASK_H_

#include "FreeRTOS.h"

inline void vTaskNotifyGiveFromISR(const TaskHandle_t task,
                                   BaseType_t* const higher_priority_task_woken) noexcept
{
	auto& m = fake_freertos::model();
	m.last_notified = task;
	++m.notifications_from_isr;
	++m.pending_count;
	if (higher_priority_task_woken != nullptr) {
		*higher_priority_task_woken = m.next_higher_priority_woken;
	}
}

// Returns the notification count before clearing (clear == pdTRUE), like the
// kernel; a zero count would block for `timeout` and is reported as 0 here.
inline uint32_t ulTaskNotifyTake(const BaseType_t clear_on_exit, const TickType_t timeout) noexcept
{
	auto& m = fake_freertos::model();
	m.last_take_timeout = timeout;
	m.last_take_clear = clear_on_exit;
	const unsigned taken = m.pending_count;
	if (clear_on_exit != pdFALSE) {
		m.pending_count = 0;
	} else if (m.pending_count != 0) {
		--m.pending_count;
	}
	return taken;
}

#endif /* FAKE_FREERTOS_TASK_H_ */
