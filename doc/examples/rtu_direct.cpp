// INTEGRATION.md §5 verbatim: the burst endpoint wired directly, and the
// framed skeleton with its own stale rule.
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "modbus/rtu/Rtu.h"
#include "platform_fake.h"
#include "Test.h"
#include <cstdio>
#include <vector>

using Serial = Uart<256, 4>;
using Link = modbus::rtu::Endpoint<wire::Pool<8, 2>>;   // framing::None

static Serial serial;
static Link link;
static unsigned g_served = 0;
static void serve(const Link::Packet&) noexcept { ++g_served; }

bool start() noexcept
{
	serial.setRxHandler(Serial::RxHandler{
		[](std::span<const uint8_t> burst) noexcept { link.receive_adu(burst); }});
	serial.setRxGapHandler(Serial::GapHandler{
		[]() noexcept { link.notify_gap(); }});
	return serial.init(&huart3) &&
	       link.bind(Link::Sender{tiny::bind<&Serial::send>(serial)},
	                 Link::BusyQuery{tiny::bind<&Serial::tx_busy>(serial)});
}

void loop_step() noexcept
{
	const uint32_t now = HAL_GetTick();
	serial.proceed(now);
	link.poll(now);
	while (auto request = link.pop_packet()) { serve(request); }
}

// ---- the framed skeleton ----
namespace framing = modbus::rtu::framing;
using Framed = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
                                     framing::Standard<framing::Direction::Request>>;
static Framed flink;
constexpr std::size_t kChunkSize = 256;   // the ChunkSize of Serial
static uint32_t g_now = 0, g_deadline = 0;
static bool g_armed = false;

static uint32_t chunk_time_ms_at_current_baud() noexcept
{
	const uint32_t baud = serial.instance()->Init.BaudRate;
	const uint32_t bit_ms = static_cast<uint32_t>(kChunkSize) * 12u * 1000u;
	return bit_ms / baud + ((bit_ms % baud) != 0u ? 1u : 0u);
}

void on_rx(std::span<const uint8_t> bytes) noexcept
{
	flink.consume(bytes);
	g_armed = flink.assembling();
	g_deadline = g_now + (bytes.size() < kChunkSize ? 5u : chunk_time_ms_at_current_baud() + 5u);
}

void framed_loop_step() noexcept
{
	g_now = HAL_GetTick();
	const bool resumed = g_armed && int32_t(g_now - g_deadline) >= 0 && serial.rx_progress() != 0;
	serial.proceed(g_now);                                  // may run on_rx / on_gap
	if (g_armed && int32_t(g_now - g_deadline) >= 0) {
		if (resumed) { g_deadline = g_now + chunk_time_ms_at_current_baud() + 5u; }
		else         { g_armed = false; flink.expire_incomplete(); }
	}
	flink.poll(g_now);
}

int main()
{
	fake::reset();
	configure_huart3(115200u);
	if (!start()) { std::puts("start failed"); return 1; }
	const auto adu = modbus_test::make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x00u, 0x6Bu, 0x00u, 0x03u});
	fake::rx_bytes(adu.data(), adu.size());
	fake::rx_idle();
	loop_step();
	const bool burst_ok = g_served == 1u;

	// Re-point the driver at the framed skeleton and run its stale rule once:
	// half a frame, IDLE, 5 ms of nothing -> expired.
	serial.setRxHandler(Serial::RxHandler{[](std::span<const uint8_t> chunk) noexcept { on_rx(chunk); }});
	serial.setRxGapHandler(Serial::GapHandler{[]() noexcept { g_armed = false; flink.notify_gap(); }});
	fake::rx_bytes(adu.data(), 3u);
	fake::rx_idle();
	framed_loop_step();
	const bool armed = g_armed && flink.assembling();
	fake::advance_tick(5u);
	framed_loop_step();
	const bool expired = !flink.assembling() && flink.framing_stats().stale_frames == 1u;
	const bool ok = burst_ok && armed && expired && fake::model().violations.empty();
	std::printf("rtu_direct: burst=%d armed=%d expired=%d -> %s\n", burst_ok, armed, expired, ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
