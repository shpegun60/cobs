/* Author: shpegun60; SPDX-License-Identifier: MIT */
// A bounded RTU server composition; host platform symbols are test scaffolding.
#define UART_ENGINE_IMPLEMENT          // in exactly one translation unit
#include "Uart.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/rtu/UartAdapter.h"
#include "platform_fake.h"
#include "Test.h"
#include "Example.h"
#include <cstdio>
#include <vector>

namespace framing = modbus::rtu::framing;
using Serial = Uart<256, 4>;
using Server = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
                                     framing::Standard<framing::Direction::Request>>;

static Serial serial;                                    // section attribute omitted on the host
static Server g_endpoint;
static modbus::rtu::UartAdapter adapter{serial, g_endpoint};   // takes no configuration: safe before main()
static Server::Message pending; // survives Busy; destroyed before the endpoint

static bool build_reply(Server::Message& reply, const Server::Packet& request) noexcept
{
	// Three holding registers: the byte count first, as a 0x03 response is
	// laid out; a count that disagrees with the data is refused before the wire.
	std::size_t offset = 0;
	uint16_t start_register = 0, count = 0;
	if (request.address() != 0x11u || request.function() != 0x03u ||
	    !modbus::read_be(request.data(), offset, start_register) ||
	    !modbus::read_be(request.data(), offset, count) || offset != request.size() ||
	    start_register != 0x006Bu || count != 3u) {
		return false; // this small demo implements only this one register range
	}
	return reply.append_be<uint8_t>(6u) && reply.append_be<uint16_t>(0x022Bu) &&
	       reply.append_be<uint16_t>(0x0000u) && reply.append_be<uint16_t>(0x0064u);
}

bool start() noexcept
{
	return serial.init(&huart3) && adapter.bind();
}

static unsigned g_replies = 0;

// example-begin: rtu-split-service
void service_transport() noexcept
{
#if DOC_SPLIT
	const uint32_t now = HAL_GetTick(); // one clock sample for all four calls
	adapter.prepare(now);              // baud refresh and DMA-progress snapshot
	serial.proceed(now);               // deliver queued RX/gaps BEFORE judging expiry
	adapter.finish(now);               // judge the incomplete frame after delivery
	g_endpoint.poll(now);              // finish() alone does not reclaim TX
#else
	adapter.proceed();                 // ordinary application: the same steps internally
#endif
}
// example-end: rtu-split-service

void loop_step() noexcept
{
	service_transport();
	if (!pending) {
		if (auto request = g_endpoint.pop_packet()) {
			pending = g_endpoint.make_message(request.address(), request.function());
			if (!pending || !build_reply(pending, request)) { pending = {}; }
		}
	}
	if (pending) {
		const auto result = g_endpoint.send(pending);
		if (result == modbus::SendResult::Sent) { ++g_replies; }
		else if (result == modbus::SendResult::Invalid || result == modbus::SendResult::Unbound) {
			pending = {}; // explicit application drop policy; Busy/Failed stay pending
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
	bool ok = g_replies == 1u && serial.tx_busy() && g_endpoint.tx_active();
	// A second request arrives before TX completes. Its Message must survive Busy.
	fake::rx_bytes(adu.data(), adu.size());
	fake::rx_idle();
	loop_step();
	ok = ok && pending && g_replies == 1u;
	fake::tx_done();
	loop_step();
	ok = ok && !pending && g_replies == 2u && g_endpoint.tx_active();
	fake::tx_done();
	loop_step();
	// Both ordinary and split servicing must deliver a queued continuation at the deadline.
	fake::rx_bytes(adu.data(), 3u); fake::rx_idle(); loop_step();
	CHECK(g_endpoint.assembling() && adapter.deadline_in_ms() == 5u);
	fake::advance_tick(5u);
	fake::rx_bytes(adu.data() + 3u, adu.size() - 3u); fake::rx_idle(); loop_step();
	CHECK(g_replies == 3u && !g_endpoint.assembling());
	fake::tx_done(); loop_step();
	// A nonzero but frozen DMA count is not new progress on every deadline.
	fake::rx_bytes(adu.data(), 3u); fake::rx_idle(); loop_step();
	fake::advance_tick(4u); adapter.on_rx({});
	CHECK(adapter.deadline_in_ms() == 1u);
	fake::rx_bytes(adu.data() + 3u, 1u);
	fake::advance_tick(1u); loop_step();
	CHECK(g_endpoint.assembling() && adapter.deadline_in_ms() > 0u);
	fake::advance_tick(adapter.deadline_in_ms()); loop_step();
	CHECK(!g_endpoint.assembling() && g_endpoint.framing_stats().stale_frames == 1u);
	fake::rx_idle(); loop_step();
	CHECK(adapter.unbind()); // releases any late, incomplete tail without counting it stale
	std::printf("rtu_adapter: replies=%u released=%d violations=%zu -> %s\n", g_replies,
	            !g_endpoint.tx_active(), fake::model().violations.size(), ok && !g_endpoint.tx_active() ? "ok" : "FAIL");
	return ok && !g_endpoint.tx_active() && fake::model().violations.empty() ? 0 : 1;
}
