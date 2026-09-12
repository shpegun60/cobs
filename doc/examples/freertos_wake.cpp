/* Author: shpegun60; SPDX-License-Identifier: MIT */
// The task's own startup attaches wake before enabling UART. The host calls
// startup/iteration explicitly: the recording fake does not schedule tasks.
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/rtu/UartAdapter.h"
#include "adapters/freertos/FreeRtosWake.h"
#include "platform_fake.h"
#include <algorithm>
#include "Test.h"
#include <cstdio>

namespace framing = modbus::rtu::framing;
using Serial = Uart<256, 4>;
using Server = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
                                     framing::Standard<framing::Direction::Request>>;
static Serial serial;
static Server g_endpoint;
static modbus::rtu::UartAdapter adapter{serial, g_endpoint};

// --- stand-in for the task handle; real task entry obtains its own handle ---
static tskTaskControlBlock* const kFakeTask = reinterpret_cast<tskTaskControlBlock*>(0x20001000u);
static unsigned g_served = 0;
static void serve(const Server::Packet&) noexcept { ++g_served; }
// ------------------------------------------------------------

static uart::FreeRtosWake wake;                    // takes no task: safe at static-init time

static void comm_task_iteration() noexcept        // one pass of the for(;;) body below
{
	(void)uart::FreeRtosWake::wait(adapter);
	adapter.proceed();
	while (auto request = g_endpoint.pop_packet()) {
		serve(request);
	}
}

bool start_comm(TaskHandle_t self) noexcept
{
	return wake.attach(serial, self) && serial.init(&huart3) && adapter.bind();
}

int main()
{
	fake::reset();
	fake_freertos::reset();
	configure_huart3(115200u);
	if (!start_comm(kFakeTask)) { std::puts("start failed"); return 1; }
	const bool null_refused = !uart::FreeRtosWake{}.attach(serial, nullptr);
	const auto adu = modbus_test::make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x00u, 0x6Bu, 0x00u, 0x03u});
	fake::rx_bytes(adu.data(), adu.size());
	fake::rx_idle();
	const auto& rtos = fake_freertos::model();
	const bool notified = rtos.notifications_from_isr == 1u && rtos.last_notified == kFakeTask;
	comm_task_iteration();
	const bool ok = null_refused && notified && g_served == 1u && rtos.last_take_timeout == 50u &&
	                fake::model().violations.empty();
	std::printf("freertos_wake: notified=%d served=%u wait=%u -> %s\n", notified, g_served,
	            static_cast<unsigned>(rtos.last_take_timeout), ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
