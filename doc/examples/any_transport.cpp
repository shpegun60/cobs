// INTEGRATION.md §6 verbatim: any byte transport, no driver, no fake HAL.
#include "modbus/rtu/Rtu.h"
#include "Cobs.h"
#include "Test.h"
#include <cstdio>
#include <vector>

static std::vector<uint8_t> g_written;
static bool write_all(std::span<const uint8_t> frame) noexcept
{
	g_written.assign(frame.begin(), frame.end());
	return true;
}

// A transport that copies the frame (sockets, QSerialPort::write) may
// report busy() == false at once; one that borrows the memory (DMA) must
// report busy until it is done with it. The endpoint releases the frame in
// poll() either way.
struct Transport final {
	bool send(std::span<const uint8_t> frame) noexcept { return write_all(frame); }
	[[nodiscard]] bool busy() const noexcept { return false; }
};

namespace framing = modbus::rtu::framing;
using RtuClient = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>,
                                        framing::Standard<framing::Direction::Response>>;
using CobsLink = cobs::Endpoint<>;   // wire::Heap, Crc16Bitwise, 253-byte payloads

Transport transport;
RtuClient client;
CobsLink cobs_link;
static unsigned g_responses = 0;
static void handle(const RtuClient::Packet&) noexcept { ++g_responses; }

bool start() noexcept
{
	return client.bind(RtuClient::Sender{tiny::bind<&Transport::send>(transport)},
	                   RtuClient::BusyQuery{tiny::bind<&Transport::busy>(transport)}) &&
	       cobs_link.bind(CobsLink::Sender{tiny::bind<&Transport::send>(transport)},
	                      CobsLink::BusyQuery{tiny::bind<&Transport::busy>(transport)});
}

void on_bytes(std::span<const uint8_t> bytes, uint32_t now_ms) noexcept
{
	client.consume(bytes);      // a fragment, several frames, or both
	client.poll(now_ms);
	while (auto response = client.pop_packet()) { handle(response); }
}

bool read_holding(uint8_t unit, uint16_t first, uint16_t count) noexcept
{
	auto request = client.make_message(unit, 0x03);
	return request.append_be(first) && request.append_be(count) &&
	       client.send(request) == modbus::SendResult::Sent;
}

int main()
{
	if (!start()) { std::puts("start failed"); return 1; }
	const bool sent = read_holding(0x11u, 0x006Bu, 3u) && g_written.size() == 8u &&
	                  g_written[0] == 0x11u && g_written[1] == 0x03u;
	client.poll(1u);
	const bool released = !client.tx_active();
	// A response 11 03 06 <6 data bytes> + CRC, delivered in two cuts.
	const auto resp = modbus_test::make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x06u, 0x02u, 0x2Bu, 0x00u, 0x00u, 0x00u, 0x64u});
	const std::span<const uint8_t> bytes{resp};
	on_bytes(bytes.first(4u), 2u);
	on_bytes(bytes.subspan(4u), 3u);
	const bool ok = sent && released && g_responses == 1u && !client.assembling();
	std::printf("any_transport: sent=%d released=%d responses=%u -> %s\n", sent, released, g_responses, ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
