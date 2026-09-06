// INTEGRATION.md §3 verbatim (platform symbols from platform_fake.h).
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "Cobs.h"
#include "platform_fake.h"
#include <cstdio>
#include <string>

using Serial = Uart<256, 4>;
using Link = cobs::Endpoint<wire::Pool<8, 2>, cobs::Format<crc::Crc16Bitwise, 1024>>;

static Serial serial;
static Link link;
static std::string g_seen;

static void handle(std::span<const uint8_t> payload) noexcept
{
	g_seen.assign(payload.begin(), payload.end());
}

bool start() noexcept
{
	serial.setRxHandler(Serial::RxHandler{
		[](std::span<const uint8_t> bytes) noexcept { link.consume(bytes); }});
	serial.setRxGapHandler(Serial::GapHandler{
		[]() noexcept { link.notify_gap(); }});
	// Either order works here: COBS needs nothing from the handle.
	return link.bind(Link::Sender{tiny::bind<&Serial::send>(serial)},
	                 Link::BusyQuery{tiny::bind<&Serial::tx_busy>(serial)}) &&
	       serial.init(&huart3);
}

void loop_step() noexcept
{
	const uint32_t now = HAL_GetTick();
	serial.proceed(now);   // the RX and gap handlers run here, in stream order
	link.poll(now);      // returns a transmitted block once the driver stops borrowing it
	while (auto packet = link.pop_packet()) {
		handle(packet.data());
	}
}

int main()
{
	fake::reset();
	configure_huart3(115200u);
	if (!start()) { std::puts("start failed"); return 1; }
	// Encode a frame with the library itself and feed it back as received bytes.
	auto message = link.make_message(5u);
	const std::string body = "hello";
	if (!message.append_bytes(std::span<const uint8_t>{
			reinterpret_cast<const uint8_t*>(body.data()), body.size()}) ||
	    link.send(message) != cobs::SendResult::Sent) {
		std::puts("send failed"); return 1;
	}
	std::string frame(reinterpret_cast<const char*>(fake::model().tx_src), fake::model().tx_len);
	fake::tx_done();
	loop_step();                                        // releases the TX block
	fake::rx_bytes(frame.data(), frame.size());
	fake::rx_idle();
	loop_step();
	const bool ok = g_seen == body && !link.tx_active() && fake::model().violations.empty();
	std::printf("cobs_direct: seen='%s' -> %s\n", g_seen.c_str(), ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
