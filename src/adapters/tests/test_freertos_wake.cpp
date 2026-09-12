/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * adapters/freertos/FreeRtosWake.h on the real driver and the recording fakes of the HAL
 * and of FreeRTOS: a wake object is built without a task and refuses a null
 * handle, every ISR event with work becomes exactly one task notification,
 * several before the task runs coalesce into one take, the yield request
 * follows what the kernel reports, and the fallback timeout is passed
 * through.
 */
#define UART_ENGINE_IMPLEMENT
#include "uart_test_fixture.h"
#include "adapters/freertos/FreeRtosWake.h"

#include <cstdio>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(const bool ok, const std::string& what)
{
	++g_checks;
	std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
	if (!ok) {
		++g_failures;
	}
}

tskTaskControlBlock* const kTask = reinterpret_cast<tskTaskControlBlock*>(0x2000'1000u);

} // namespace

int main()
{
	std::printf("\n[FreeRtosWake]\n");
	fake::reset();
	fake_freertos::reset();
	Fixture f;
	f.start();
	auto& rtos = fake_freertos::model();

	// The static-init situation: the object exists before the task does.
	uart::FreeRtosWake wake;
	check(wake.task() == nullptr, "constructed without a task, as a static object before xTaskCreate()");
	check(!wake.attach(f.uart, nullptr) && wake.task() == nullptr,
	      "attach() refuses a null task handle");
	fake::rx_bytes("no", 2);
	fake::rx_idle();
	check(rtos.notifications_from_isr == 0, "and installed nothing: the chunk notified nobody");
	f.loop();
	check(rxText() == "no", "the chunk is still delivered by the loop");

	check(wake.attach(f.uart, kTask) && wake.task() == kTask,
	      "attach() with the real handle installs the wake and remembers the task");
	fake::rx_bytes("abc", 3);
	fake::rx_idle();
	check(rtos.notifications_from_isr == 1 && rtos.last_notified == kTask,
	      "an RX chunk becomes one vTaskNotifyGiveFromISR() to the task");
	check(rtos.yields_not_requested == 1 && rtos.yields_requested == 0,
	      "no yield is requested while the kernel reports no higher-priority task woken");

	rtos.next_higher_priority_woken = pdTRUE;
	fake::rx_bytes("de", 2);
	fake::rx_idle();
	fake::rx_bytes("f", 1);
	fake::rx_idle();
	check(rtos.notifications_from_isr == 3 && rtos.yields_requested == 2,
	      "each further chunk notifies once and requests the yield the kernel asked for");
	check(rtos.pending_count == 3, "three notifications are pending before the task runs");

	const uint32_t taken = uart::FreeRtosWake::wait(50u);
	check(taken == 3 && rtos.pending_count == 0 && rtos.last_take_clear == pdTRUE,
	      "one wait() takes all pending notifications: ISR bursts coalesce into one proceed()");
	check(rtos.last_take_timeout == 50u, "the fallback timeout reaches ulTaskNotifyTake in ticks");
	f.loop();
	check(rxText() == "noabcdef", "the woken loop delivers every chunk in order");

	const uint8_t frame[4] = {1, 2, 3, 0};
	check(f.uart.send(std::span<const uint8_t>{frame, 4}), "send starts a transfer");
	fake::tx_done();
	check(rtos.notifications_from_isr == 4, "TX completion notifies the task to reclaim the frame");

	fake::rx_bytes("zz", 2);
	fake::rx_error(HAL_UART_ERROR_ORE);
	check(rtos.notifications_from_isr == 5, "an RX error notifies the task for recovery");
	check(uart::FreeRtosWake::wait(100u) == 2 && rtos.last_take_timeout == 100u,
	      "the two pending notifications are taken together");
	f.loop();
	check(fake::model().violations.empty(), "no ownership violation with the wake attached");

	struct Deadline {
		uint32_t remaining = UINT32_MAX;
		mutable uint32_t observed_now = 0u;
		uint32_t deadline_in_ms(uint32_t now) const noexcept { observed_now = now; return remaining; }
	} deadline;
	check(uart::FreeRtosWake::wait(deadline, 123u) == 0u && rtos.last_take_timeout == 50u &&
	      deadline.observed_now == 123u, "adapter wait uses default fallback and forwards the caller's clock");
	deadline.remaining = 7u;
	check(uart::FreeRtosWake::wait(deadline, UINT32_MAX) == 0u && rtos.last_take_timeout == 7u &&
	      deadline.observed_now == UINT32_MAX, "nearer deadline wins, including a wrapping caller timestamp");
	deadline.remaining = 0u;
	check(uart::FreeRtosWake::wait(deadline, 0u) == 0u && rtos.last_take_timeout == 0u,
	      "an expired deadline never blocks the communication task");
	deadline.remaining = 100u;
	check(uart::FreeRtosWake::wait(deadline, 0u) == 0u && rtos.last_take_timeout == 50u,
	      "fallback still bounds a later protocol deadline");
	check(uart::FreeRtosWake::wait(deadline, 0u, 3u) == 0u && rtos.last_take_timeout == 3u,
	      "an explicit smaller application fallback is honoured");
	check(uart::FreeRtosWake::wait(deadline, 0u, 0u) == 0u && rtos.last_take_timeout == 0u,
	      "zero application fallback is nonblocking");

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
