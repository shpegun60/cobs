/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * uart/FreeRtosWake.h on the real driver and the recording fakes of the HAL
 * and of FreeRTOS: every ISR event with work becomes exactly one task
 * notification, several before the task runs coalesce into one take, the
 * yield request follows what the kernel reports, and the fallback timeout is
 * passed through.
 */
#define UART_ENGINE_IMPLEMENT
#include "uart_test_fixture.h"
#include "FreeRtosWake.h"

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
	uart::FreeRtosWake wake{kTask};
	check(wake.task() == kTask, "the wake object remembers the communication task");
	wake.attach(f.uart);

	auto& rtos = fake_freertos::model();
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
	check(rxText() == "abcdef", "the woken loop delivers every chunk in order");

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

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
