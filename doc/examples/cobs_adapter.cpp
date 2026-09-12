/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

// INTEGRATION.md section 3: the same setup/service calls as the RTU adapter.
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "cobs/Cobs.h"
#include "adapters/cobs/UartAdapter.h"
#include "platform_fake.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

using Serial = Uart<256, 4>;
using Link = cobs::Endpoint<wire::Pool<8, 2>, cobs::Format<crc::Crc16Bitwise, 1024>>;
static Serial serial;
static Link g_endpoint;
static cobs::UartAdapter adapter{serial, g_endpoint};

bool start() noexcept { return serial.init(&huart3) && adapter.bind(); }
void loop_step() noexcept { adapter.proceed(); }

int main()
{
	fake::reset();
	configure_huart3(115200u);
	if (!start()) { std::puts("start failed"); return 1; }
	constexpr std::array<uint8_t, 4> body{1u, 0u, 2u, 3u};
	auto message = g_endpoint.make_message(body.size());
	if (!message.append_bytes(body) || g_endpoint.send(message) != cobs::SendResult::Sent) {
		std::puts("send failed"); return 1;
	}
	const std::vector<uint8_t> frame(fake::model().tx_src, fake::model().tx_src + fake::model().tx_len);
	fake::tx_done();
	loop_step();
	fake::rx_bytes(frame.data(), frame.size());
	fake::rx_idle();
	loop_step();
	auto packet = g_endpoint.pop_packet();
	const bool ok = packet && std::ranges::equal(packet.data(), body) &&
		!g_endpoint.tx_active() && fake::model().violations.empty();
	std::printf("cobs_adapter: %s\n", ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
