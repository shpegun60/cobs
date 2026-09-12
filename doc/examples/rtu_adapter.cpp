// INTEGRATION.md §2 verbatim (platform symbols from platform_fake.h).
#define UART_ENGINE_IMPLEMENT          // in exactly one translation unit
#include "Uart.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/rtu/UartAdapter.h"
#include "platform_fake.h"
#include "Test.h"
#include <cstdio>
#include <vector>

namespace framing = modbus::rtu::framing;
using Serial = Uart<256, 4>;
using Server = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
                                     framing::Standard<framing::Direction::Request>>;

static Serial serial;                                    // section attribute omitted on the host
static Server g_endpoint;
static modbus::rtu::UartAdapter adapter{serial, g_endpoint};   // takes no configuration: safe before main()

static bool build_reply(Server::Message& reply, const Server::Packet& request) noexcept
{
	// Three holding registers: the byte count first, as a 0x03 response is
	// laid out; a count that disagrees with the data is refused before the wire.
	(void)request;
	return reply.append_be<uint8_t>(6u) && reply.append_be<uint16_t>(0x022Bu) &&
	       reply.append_be<uint16_t>(0x0000u) && reply.append_be<uint16_t>(0x0064u);
}

bool start() noexcept
{
	return serial.init(&huart3) && adapter.bind();
}

static unsigned g_replies = 0;

void loop_step() noexcept
{
	adapter.proceed();   // uart.proceed -> frame verdict -> g_endpoint.poll
	while (auto request = g_endpoint.pop_packet()) {
		auto reply = g_endpoint.make_message(request.address(), request.function());
		if (!build_reply(reply, request)) {
			continue;
		}
		if (g_endpoint.send(reply) == modbus::SendResult::Sent) {
			++g_replies;
		}
	}
}

int main()
{
	fake::reset();
	configure_huart3(115200u);
	if (!start()) { std::puts("start failed"); return 1; }
	// A read-holding request 11 03 00 6B 00 03 + CRC (CRC computed by the library's policy).
	const auto adu = modbus_test::make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x00u, 0x6Bu, 0x00u, 0x03u});
	fake::rx_bytes(adu.data(), adu.size());
	fake::rx_idle();
	loop_step();
	const bool ok = g_replies == 1u && serial.tx_busy() && g_endpoint.tx_active();
	fake::tx_done();
	loop_step();
	std::printf("rtu_adapter: replies=%u released=%d violations=%zu -> %s\n", g_replies,
	            !g_endpoint.tx_active(), fake::model().violations.size(), ok && !g_endpoint.tx_active() ? "ok" : "FAIL");
	return ok && !g_endpoint.tx_active() && fake::model().violations.empty() ? 0 : 1;
}
