/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Manual binding, no protocol adapter. DOC_WAKE adds the same FreeRTOS wake.
#define UART_ENGINE_IMPLEMENT
#include "uart/Uart.h"
#include "cobs/Cobs.h"
#include "platform_fake.h"
#include <cstdio>
#include <algorithm>
#include <array>
#ifdef DOC_WAKE
#include "adapters/freertos/FreeRtosWake.h"
static uart::FreeRtosWake wake;
#endif

using Serial = Uart<256, 4>;
using Link = cobs::Endpoint<wire::Pool<8, 2>, cobs::Format<crc::Crc16Bitwise, 1024>>;

static Serial serial;
static Link g_endpoint;
static std::array<uint8_t, Link::max_receive_size> g_seen{};
static std::size_t g_seen_size = 0;

static void handle(std::span<const uint8_t> payload) noexcept
{
	if (payload.size() <= g_seen.size()) {
		std::copy(payload.begin(), payload.end(), g_seen.begin());
		g_seen_size = payload.size();
	}
}

bool start() noexcept
{
	serial.setRxHandler(Serial::RxHandler{
		[](std::span<const uint8_t> bytes) noexcept { g_endpoint.consume(bytes); }});
	serial.setRxGapHandler(Serial::GapHandler{
		[]() noexcept { g_endpoint.notify_gap(); }});
	// Either order works here: COBS needs nothing from the handle.
	return g_endpoint.bind(Link::Sender{tiny::bind<&Serial::send>(serial)},
	                       Link::BusyQuery{tiny::bind<&Serial::tx_busy>(serial)}) &&
	       serial.init(&huart3);
}

void loop_step() noexcept
{
#ifdef DOC_WAKE
	(void)uart::FreeRtosWake::wait(50u);
#endif
	const uint32_t now = HAL_GetTick();
	serial.proceed(now);   // the RX and gap handlers run here, in stream order
	g_endpoint.poll(now);    // returns a transmitted block once the driver stops borrowing it
	while (auto packet = g_endpoint.pop_packet()) {
		handle(packet.data());
	}
}

int main()
{
	fake::reset();
	configure_huart3(115200u);
#ifdef DOC_WAKE
	fake_freertos::reset();
	if (!wake.attach(serial, reinterpret_cast<TaskHandle_t>(0x20001000u))) { return 1; }
#endif
	if (!start()) { std::puts("start failed"); return 1; }
	// Encode a frame with the library itself and feed it back as received bytes.
	auto message = g_endpoint.make_message(5u);
	const std::array<uint8_t, 5> body{'h', 'e', 'l', 'l', 'o'};
	if (!message || !message.append_bytes(body) ||
	    g_endpoint.send(message) != cobs::SendResult::Sent) {
		std::puts("send failed"); return 1;
	}
	std::array<uint8_t, Link::Geometry::tx_block_bytes> frame{};
	const std::size_t frame_size = fake::model().tx_len;
	std::copy_n(fake::model().tx_src, frame_size, frame.begin());
	fake::tx_done();
	loop_step();                                        // releases the TX block
	fake::rx_bytes(frame.data(), frame_size);
	fake::rx_idle();
	loop_step();
	const bool ok = g_seen_size == body.size() && std::equal(body.begin(), body.end(), g_seen.begin()) &&
		!g_endpoint.tx_active() && fake::model().violations.empty();
	std::printf("cobs_direct: received=%zu -> %s\n", g_seen_size, ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
