/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

// The INTEGRATION.md task loop, unchanged, with the COBS adapter.
// Host-only proof using the recording FreeRTOS fake, not a real scheduler.
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "cobs/Cobs.h"
#include "adapters/cobs/UartAdapter.h"
#include "adapters/freertos/FreeRtosWake.h"
#include "platform_fake.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

using Serial = Uart<256, 4>;
using Link = cobs::Endpoint<wire::Pool<8, 2>>;
static uart::FreeRtosWake wake;
static Serial serial;
static Link g_endpoint;
static cobs::UartAdapter adapter{serial, g_endpoint};

static void task_iteration() noexcept
{
	(void)uart::FreeRtosWake::wait(adapter);
	adapter.proceed();
}

int main()
{
	fake::reset();
	fake_freertos::reset();
	configure_huart3(115200u);
	auto* const task = reinterpret_cast<tskTaskControlBlock*>(0x20001000u);
	if (!(serial.init(&huart3) && adapter.bind() && wake.attach(serial, task))) {
		std::puts("start failed"); return 1;
	}
	constexpr std::array<uint8_t, 4> body{0x12u, 0u, 0x34u, 0u};
	auto message = g_endpoint.make_message(body.size());
	if (!message.append_bytes(body) || g_endpoint.send(message) != cobs::SendResult::Sent) {
		std::puts("send failed"); return 1;
	}
	const std::vector<uint8_t> frame(fake::model().tx_src, fake::model().tx_src + fake::model().tx_len);
	fake::tx_done();
	fake::rx_bytes(frame.data(), frame.size());
	fake::rx_idle();
	const bool deferred = !g_endpoint.has_packet() && g_endpoint.tx_active() &&
		fake_freertos::model().pending_count == 2u;
	task_iteration();
	auto packet = g_endpoint.pop_packet();
	const auto& rtos = fake_freertos::model();
	const bool ok = deferred && packet && std::ranges::equal(packet.data(), body) &&
		!g_endpoint.tx_active() && rtos.last_notified == task && rtos.pending_count == 0u &&
		rtos.last_take_timeout == 50u && fake::model().violations.empty();
	std::printf("cobs_freertos: deferred=%d, same wake/task loop -> %s\n", deferred, ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
