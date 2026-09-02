/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"
#include "Test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace modbus_test;

namespace {

struct Transport final {
	bool busy_state = false;
	bool accept = true;
	std::vector<uint8_t> frame{};

	bool send(const std::span<const uint8_t> bytes) noexcept
	{
		if (busy_state || !accept) {
			return false;
		}
		frame.assign(bytes.begin(), bytes.end());
		busy_state = true;
		return true;
	}

	bool busy() const noexcept { return busy_state; }
};

} // namespace

int main()
{
	using Endpoint = modbus::rtu::Endpoint<modbus::rtu::Pool<2, 1>>;
	Endpoint endpoint;

	group("ReceiveValidation");
	endpoint.receive_adu({});
	const std::array<uint8_t, 3> short_frame{1u, 3u, 0u};
	endpoint.receive_adu(short_frame);
	std::vector<uint8_t> oversize(257u, 0u);
	endpoint.receive_adu(oversize);
	auto corrupted = make_adu(1u, 3u, std::array<uint8_t, 2>{0u, 1u});
	corrupted[1] ^= 0x01u;
	endpoint.receive_adu(corrupted);
	check(!endpoint.has_packet(), "invalid candidates never publish a Packet");
	auto stats = endpoint.stats();
	check(stats.rx.candidates == 4u && stats.rx.too_short == 2u &&
	      stats.rx.oversize == 1u && stats.rx.crc_errors == 1u &&
	      stats.rx.frames_received == 0u,
	      "every rejection has one unambiguous diagnostic counter");

	group("ReceiveOrdering");
	const auto first = make_adu(1u, 0x03u, std::array<uint8_t, 2>{0x12u, 0x34u});
	const auto second = make_adu(2u, 0x64u, std::array<uint8_t, 3>{0u, 1u, 0u});
	endpoint.receive_adu(first);
	endpoint.receive_adu(second);
	check(endpoint.has_packet(), "valid candidates enter the ready queue");
	auto a = endpoint.pop_packet();
	auto b = endpoint.pop_packet();
	check(a && b && a.address() == 1u && b.address() == 2u &&
	      a.function() == 3u && b.function() == 0x64u,
	      "Packets leave in candidate order with metadata intact");
	check(!endpoint.pop_packet(), "ready queue is empty after both ownership transfers");
	endpoint.notify_gap();
	check(endpoint.stats().rx.stream_gaps == 1u,
	      "ordered UART discontinuity is recorded without inventing a frame");
	a.reset();
	b.reset();

	group("BindingAndIdentity");
	Transport transport;
	check(!endpoint.bind({}, {}), "two empty transport delegates are refused");
	check(!endpoint.bind(Endpoint::Sender{tiny::bind<&Transport::send>(transport)}, {}),
	      "a half-bound transport is refused transactionally");
	check(endpoint.bind(
		Endpoint::Sender{tiny::bind<&Transport::send>(transport)},
		Endpoint::BusyQuery{tiny::bind<&Transport::busy>(transport)}),
	      "complete sender/busy pair binds");

	auto message = endpoint.make_message(1u, 6u, 2u);
	check(message.append_be(uint16_t{0xBEEFu}), "message is built through public API");
	Endpoint other;
	check(other.send(message) == modbus::SendResult::Invalid && message,
	      "same-typed foreign Endpoint cannot steal a Message block");
	check(endpoint.send(message) == modbus::SendResult::Sent && !message,
	      "creating Endpoint accepts its Message");
	check(!endpoint.unbind(), "transport cannot be unbound during an active borrow");
	transport.busy_state = false;
	endpoint.poll();
	check(endpoint.unbind(), "transport unbinds after ownership is reclaimed");
	auto unbound = endpoint.make_message(1u, 6u, 0u);
	check(endpoint.send(unbound) == modbus::SendResult::Unbound && unbound,
	      "Unbound keeps the caller's Message");

	group("Snapshot");
	stats = endpoint.stats();
	check(stats.tx.frames_sent == 1u && stats.tx.send_refused_busy == 0u &&
	      stats.tx.send_failed == 0u,
	      "Stats returns one RX/TX value snapshot");

	return finish();
}
