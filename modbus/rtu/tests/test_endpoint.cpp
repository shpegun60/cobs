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
#include <type_traits>
#include <utility>
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

struct SumState final {
	unsigned calls = 0u;
	std::size_t last_size = 0u;
};

struct WrappingSum final {
	explicit WrappingSum(SumState& value) noexcept : state(&value) {}
	WrappingSum() = delete;

	SumState* state;

	[[nodiscard]] uint16_t calculate(
			const std::span<const uint8_t> bytes) noexcept
	{
		++state->calls;
		state->last_size = bytes.size();
		uint16_t value = 0u;
		for (const uint8_t byte : bytes) {
			value = static_cast<uint16_t>(
				value + static_cast<uint16_t>(byte));
		}
		return value;
	}
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

	group("TablePolicy");
	using TableEndpoint = modbus::rtu::Endpoint<
		modbus::rtu::Pool<2, 1>, modbus::rtu::crc::Table>;
	TableEndpoint table_endpoint;
	Transport table_transport;
	check(table_endpoint.bind(
		TableEndpoint::Sender{tiny::bind<&Transport::send>(table_transport)},
		TableEndpoint::BusyQuery{tiny::bind<&Transport::busy>(table_transport)}),
	      "table endpoint binds without runtime CRC selection");
	auto table_message = table_endpoint.make_message(0x11u, 0x03u, 4u);
	check(table_message.append_be(uint16_t{0x0020u}) &&
	      table_message.append_be(uint16_t{0x0002u}) &&
	      table_endpoint.send(table_message) == modbus::SendResult::Sent,
	      "table endpoint transmits a complete ADU");
	const auto expected_table = make_adu(
		0x11u, 0x03u, std::array<uint8_t, 4>{0x00u, 0x20u, 0x00u, 0x02u});
	check(table_transport.frame == expected_table,
	      "table policy produces byte-identical standard Modbus wire CRC");
	table_endpoint.receive_adu(table_transport.frame);
	auto table_packet = table_endpoint.pop_packet();
	check(table_packet && equal(
	          table_packet.adu(), table_transport.frame),
	      "the same table policy validates RX and publishes the packet");
	table_packet.reset();
	table_transport.busy_state = false;
	table_endpoint.poll();

	group("CustomChecksumPolicy");
	using SumEndpoint = modbus::rtu::Endpoint<
		modbus::rtu::Pool<2, 1>, WrappingSum>;
	static_assert(!std::is_default_constructible_v<SumEndpoint>);
	static_assert(std::is_constructible_v<SumEndpoint, WrappingSum>);
	static_assert(std::is_constructible_v<
		SumEndpoint, WrappingSum, std::in_place_t>);
	SumState sum_state;
	SumEndpoint sum_endpoint{WrappingSum{sum_state}};
	Transport sum_transport;
	check(sum_endpoint.bind(
		SumEndpoint::Sender{tiny::bind<&Transport::send>(sum_transport)},
		SumEndpoint::BusyQuery{tiny::bind<&Transport::busy>(sum_transport)}),
	      "stateful custom calculator is injected into Endpoint");
	auto sum_message = sum_endpoint.make_message(0x21u, 0x45u, 3u);
	constexpr std::array<uint8_t, 3> sum_data{0xF0u, 0x20u, 0x03u};
	check(sum_message.append_bytes(sum_data) &&
	      sum_endpoint.send(sum_message) == modbus::SendResult::Sent,
	      "custom calculator is used for TX");
	constexpr uint16_t expected_sum = 0x0179u;
	check(sum_transport.frame.size() == 7u &&
	      sum_transport.frame[5] == static_cast<uint8_t>(expected_sum) &&
	      sum_transport.frame[6] == static_cast<uint8_t>(expected_sum >> 8u) &&
	      sum_state.calls == 1u && sum_state.last_size == 5u,
	      "RTU stores the custom 16-bit result low byte first without semantic validation");
	check(!modbus::rtu::crc::verify(sum_transport.frame),
	      "a deliberately non-Modbus checksum is not silently relabeled as CRC-16/MODBUS");
	sum_endpoint.receive_adu(sum_transport.frame);
	auto sum_packet = sum_endpoint.pop_packet();
	check(sum_packet && equal(sum_packet.data(), sum_data) &&
	      sum_state.calls == 2u && sum_state.last_size == 5u,
	      "the same stateful custom calculator validates RX without an algorithm check");
	sum_packet.reset();
	sum_transport.busy_state = false;
	sum_endpoint.poll();

	return finish();
}
