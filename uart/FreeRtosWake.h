/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * uart::FreeRtosWake — the driver's WakeHandler turned into a FreeRTOS task
 * notification, so one communication task can sleep until the UART has work
 * instead of polling proceed() on a timer.
 *
 *     Uart<256, 4> uart;
 *     uart::FreeRtosWake wake{communicationTaskHandle};
 *     wake.attach(uart);                       // uart.setWakeHandler(...)
 *
 *     void communicationTask(void*)
 *     {
 *         for (;;) {
 *             uart::FreeRtosWake::wait(50u);   // woken by the ISR, or every 50 ms
 *             adapter.proceed(HAL_GetTick());  // uart.proceed -> endpoint
 *             while (auto packet = link.pop_packet()) { handle(packet); }
 *         }
 *     }
 *
 * The driver raises the wake from its RX event, TX completion and error
 * ISRs, after its own state is final (uart/Uart.h, WakeHandler). Here that
 * becomes vTaskNotifyGiveFromISR() plus portYIELD_FROM_ISR(): several ISRs
 * before the task runs coalesce into one wake, because ulTaskNotifyTake(pdTRUE,
 * ...) clears the count. The fallback timeout in wait() is not polling: it is
 * the slow path for what no UART event announces — the driver's own health
 * audit, an endpoint's stale-frame deadline, a client's request timeout.
 * UartAdapter::deadline_in_ms() gives a tighter bound when a frame is in
 * flight.
 *
 * Integration contract, FreeRTOS on Cortex-M: the USART and DMA interrupt
 * priorities must be numerically >= configMAX_SYSCALL_INTERRUPT_PRIORITY (that
 * is, not more urgent than the kernel allows for FromISR calls); an interrupt
 * above that limit calling vTaskNotifyGiveFromISR() is undefined behaviour.
 * The communication task must be the only one that touches the UART, the
 * endpoint, its Packets and Messages: their reference counts are not atomic
 * by design.
 *
 * This header is not part of the driver; the driver knows no scheduler. It is
 * compiled in the host suite against a recording fake of the two FreeRTOS
 * headers it includes (uart/tests/host/fake_freertos), never against a real
 * kernel here.
 */

#ifndef UART_FREERTOS_WAKE_H_
#define UART_FREERTOS_WAKE_H_

#include "FreeRTOS.h"
#include "task.h"

#include "tiny_delegate.hpp"

#include <cstdint>

namespace uart {

class FreeRtosWake final {
public:
	explicit FreeRtosWake(const TaskHandle_t task) noexcept : m_task(task) {}

	FreeRtosWake(const FreeRtosWake&) = delete;
	FreeRtosWake& operator=(const FreeRtosWake&) = delete;

	// ISR side: one notification to the task, and a context switch on exit
	// when the task outranks whatever was running.
	void notify() noexcept
	{
		BaseType_t higher_priority_task_woken = pdFALSE;
		vTaskNotifyGiveFromISR(m_task, &higher_priority_task_woken);
		portYIELD_FROM_ISR(higher_priority_task_woken);
	}

	// Installs notify() as the driver's wake handler. The wake object must
	// outlive the driver's use of it (it is bound by reference, not copied).
	template<class Serial>
	void attach(Serial& serial) noexcept
	{
		serial.setWakeHandler(typename Serial::WakeHandler{
			tiny::bind<&FreeRtosWake::notify>(*this)});
	}

	// Task side: blocks until a notification arrives or fallback_ms pass;
	// returns the number of notifications taken (0 on the timeout).
	[[nodiscard]] static uint32_t wait(const uint32_t fallback_ms) noexcept
	{
		return static_cast<uint32_t>(
			ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(fallback_ms)));
	}

	[[nodiscard]] TaskHandle_t task() const noexcept { return m_task; }

private:
	TaskHandle_t m_task;
};

} // namespace uart

#endif /* UART_FREERTOS_WAKE_H_ */
