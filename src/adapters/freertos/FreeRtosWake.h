/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * uart::FreeRtosWake — the driver's WakeHandler turned into a FreeRTOS task
 * notification, so one communication task can sleep until the UART has work
 * instead of polling proceed() on a timer.
 * This is protocol-independent: the SAME object serves COBS and Modbus RTU.
 * Choose cobs::UartAdapter or modbus::rtu::UartAdapter for the protocol; wake
 * still attaches only to serial. Without a sleeping FreeRTOS task, omit it.
 *
 *     static Uart<256, 4> serial;
 *     static uart::FreeRtosWake wake;              // no task yet: safe at static-init time
 *
 *     // after xTaskCreate(communicationTask, ..., &communicationTaskHandle):
 *     wake.attach(serial, communicationTaskHandle);  // false for a null handle: nothing installed
 *
 *     void communicationTask(void*)
 *     {
 *         for (;;) {
 *             (void)uart::FreeRtosWake::wait(adapter);
 *             adapter.proceed();                   // fresh tick, uart -> endpoint
 *             while (auto packet = link.pop_packet()) { handle(packet); }
 *         }
 *     }
 *
 * The driver raises the wake from its RX event, TX completion and error
 * ISRs, after its own state is final (uart/Uart.h, WakeHandler). Here that
 * becomes vTaskNotifyGiveFromISR() plus portYIELD_FROM_ISR(): several ISRs
 * before the task runs coalesce into one wake, because ulTaskNotifyTake(pdTRUE,
 * ...) clears the count.
 *
 * The task handle is taken by attach(), not by the constructor: a CubeMX
 * `static TaskHandle_t communicationTaskHandle` is still null when static
 * initializers run, so a wake object built from it there would notify
 * nobody forever. attach() refuses a null handle and then installs nothing.
 *
 * The wait is bounded by two things. The fallback timeout is not polling:
 * it is the slow path for what no UART event announces — the driver's own
 * health audit, a client's request timeout. The transport adapter's
 * deadline_in_ms(now) is the other bound for framed RTU: a frame
 * whose remainder never comes must be expired when its deadline falls due,
 * not when the next unrelated frame wakes the task (that frame's bytes would
 * be glued onto the orphan first). deadline_in_ms() is no_deadline while
 * nothing is in flight and 0 when due. wait(adapter, fallback_ms)
 * chooses the shorter bound internally; its default fallback is 50 ms.
 * cobs::UartAdapter always returns no_deadline, so
 * that same loop uses just the fallback timeout with COBS; there is no COBS
 * timer and no protocol parsing in this ISR wake handler.
 *
 * The wait is as fine as the kernel tick. Conversion truncates to whole
 * ticks, so with a tick coarser than the deadline (configTICK_RATE_HZ
 * of 100 gives 10 ms ticks; a 5 ms deadline becomes 0 ticks) wait() returns
 * at once and the loop services proceed() back to back until HAL_GetTick()
 * reaches the deadline: correct, but a busy loop for those milliseconds.
 * Give the kernel a tick at least as fine as the deadlines the transport
 * uses (the usual 1 kHz STM32 configuration is); no timer of its own is
 * kept here to paper over a coarse one.
 *
 * Durations use a wide intermediate instead of pdMS_TO_TICKS(): the default
 * FreeRTOS macro can overflow before division, even when the result fits.
 * The result saturates at portMAX_DELAY - 1, never the indefinite-wait sentinel.
 * Application overrides of pdMS_TO_TICKS do not change this bounded contract.
 *
 * wait() acts on the CALLING task (ulTaskNotifyTake has no task argument):
 * it must be called only by the task whose handle was attached, and
 * notification index 0 of that task belongs to the UART wake — other code
 * in the task must not take from it.
 *
 * Integration contract, FreeRTOS on Cortex-M: an interrupt that calls a
 * FromISR API must not be logically more urgent than the kernel's syscall
 * ceiling. On STM32 that is written with the HAL/CMSIS number:
 * HAL_NVIC_SetPriority(USARTx_IRQn, p, 0) with
 * p >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5 in CubeMX defaults).
 * configMAX_SYSCALL_INTERRUPT_PRIORITY is the same limit already shifted
 * into the priority register's form (5 << 4 = 80 on a 4-bit NVIC): never
 * compare the HAL number against it. An interrupt above the ceiling calling
 * vTaskNotifyGiveFromISR() is undefined behaviour. The communication task
 * must be the only one that touches the UART, the endpoint, its Packets and
 * Messages: their reference counts are not atomic by design.
 *
 * This header is not part of the driver; the driver knows no scheduler. It is
 * compiled in the host suite against a recording fake of the two FreeRTOS
 * headers it includes (src/adapters/tests/fake_freertos). The separate live
 * harness in src/adapters/tests/hardware/h7s/parity uses a real FreeRTOS kernel.
 */

#ifndef UART_FREERTOS_WAKE_H_
#define UART_FREERTOS_WAKE_H_

#include "FreeRTOS.h"
#include "task.h"

#include "tiny_delegate.hpp"

#include <cstdint>
#include <concepts>

namespace uart {

namespace detail {

[[nodiscard]] constexpr TickType_t wake_ticks_for(const uint32_t milliseconds) noexcept
{
	static_assert(configTICK_RATE_HZ > 0u &&
	              static_cast<uint64_t>(configTICK_RATE_HZ) <= UINT32_MAX);
	static_assert(portMAX_DELAY > 0u);
	constexpr uint64_t maximum = static_cast<uint64_t>(portMAX_DELAY) - 1u;
	if constexpr (configTICK_RATE_HZ == 1000u) {
		// Explicitly remove the multiply/divide. Cortex-M0 GCC -Os can otherwise
		// retain a 64-bit multiply in the saturation comparison even at 1 kHz.
		return static_cast<TickType_t>(milliseconds < maximum ? milliseconds : maximum);
	} else {
		const uint64_t ticks = static_cast<uint64_t>(milliseconds) *
			static_cast<uint64_t>(configTICK_RATE_HZ) / 1000u;
		return static_cast<TickType_t>(ticks < maximum ? ticks : maximum);
	}
}

} // namespace detail

class FreeRtosWake final {
public:
	static constexpr uint32_t default_fallback_ms = 50u;
	// Takes no task: safe to construct before the scheduler or the task exist.
	FreeRtosWake() noexcept = default;

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

	// Installs notify() as the driver's wake handler for `task`. Refuses a
	// null handle (the one a static wake object would have captured before
	// xTaskCreate() ran) and then installs nothing. The wake object must
	// outlive the driver's use of it: it is bound by reference, not copied.
	template<class Serial>
	[[nodiscard]] bool attach(Serial& serial, const TaskHandle_t task) noexcept
	{
		if (task == nullptr) {
			return false;
		}
		m_task = task;
		serial.setWakeHandler(typename Serial::WakeHandler{
			tiny::bind<&FreeRtosWake::notify>(*this)});
		return true;
	}

	// Task side, called by the attached task only: blocks until a
	// notification arrives or fallback_ms pass; returns the number of
	// notifications taken (0 on the timeout). Raw-duration form for callers
	// that already own the whole wait budget; normally use the adapter form.
	[[nodiscard]] static uint32_t wait(const uint32_t fallback_ms) noexcept
	{
		return static_cast<uint32_t>(
			ulTaskNotifyTake(pdTRUE, detail::wake_ticks_for(fallback_ms)));
	}

	// The platform adapter owns its clock. This wake layer needs no HAL,
	// clock policy, timer state, allocation or callback wrapper. COBS's constant
	// no_deadline folds to the fallback at compile time. adapter.proceed()
	// subsequently reads a fresh tick: no pre-wait timestamp can stamp fresh RX.
	template<class Adapter>
		requires requires(const Adapter& adapter) {
			{ adapter.deadline_in_ms() } noexcept -> std::same_as<uint32_t>;
		}
	[[nodiscard]] static uint32_t wait(
			const Adapter& adapter,
			const uint32_t fallback_ms = default_fallback_ms) noexcept
	{
		const uint32_t remaining = adapter.deadline_in_ms();
		return wait(remaining < fallback_ms ? remaining : fallback_ms);
	}

	// The attached task; null until attach() succeeded.
	[[nodiscard]] TaskHandle_t task() const noexcept { return m_task; }

private:
	TaskHandle_t m_task = nullptr;
};

} // namespace uart

#endif /* UART_FREERTOS_WAKE_H_ */
