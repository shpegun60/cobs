/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"
#include "Test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

using namespace modbus_test;

int main()
{
	using Endpoint = modbus::rtu::Endpoint<modbus::rtu::Pool<3, 1>>;
	using Packet = Endpoint::Packet;
	static_assert(sizeof(Packet) == sizeof(void*));
	static_assert(std::is_copy_constructible_v<Packet>);
	static_assert(std::is_move_constructible_v<Packet>);

	Endpoint endpoint;

	group("Views");
	constexpr std::array<uint8_t, 5> data{0x00u, 0x12u, 0x34u, 0x00u, 0xFFu};
	const auto wire = make_adu(0x11u, 0x43u, data);
	endpoint.receive_adu(wire);
	Packet packet = endpoint.pop_packet();
	check(static_cast<bool>(packet), "a CRC-valid custom-function ADU produces a Packet");
	check(packet.address() == 0x11u && packet.function() == 0x43u,
	      "address and function are block metadata");
	check(packet.size() == data.size() && equal(packet.data(), data),
	      "data()/size() expose only function data, like COBS payload");
	check(packet.pdu().size() == data.size() + 1u && packet.pdu()[0] == 0x43u &&
	      equal(packet.pdu().subspan(1u), data),
	      "pdu() exposes function plus data");
	check(equal(packet.adu(), wire) && modbus::rtu::crc::verify(packet.adu()),
	      "adu() exposes the complete immutable wire frame");

	group("SharedOwnership");
	check(endpoint.storage().rx_available() == 2u, "one Packet holds one pool block");
	Packet copy = packet;
	packet.reset();
	check(copy && endpoint.storage().rx_available() == 2u && equal(copy.data(), data),
	      "a copy shares the block after the original resets");
	Packet moved = std::move(copy);
	check(!copy && moved, "move transfers the same reference without incrementing it");
	moved.reset();
	check(endpoint.storage().rx_available() == 3u,
	      "the last shared handle returns the block exactly once");

	group("SelfAssignment");
	endpoint.receive_adu(wire);
	Packet self = endpoint.pop_packet();
	Packet& alias = self;
	self = alias;
	check(self && equal(self.data(), data),
	      "self copy-assignment preserves the handle and ADU");
	self = std::move(alias);
	check(self && equal(self.data(), data),
	      "self move-assignment preserves the handle and ADU");
	self.reset();
	check(endpoint.storage().rx_available() == 3u &&
	      endpoint.storage().rx_stats().rejected == 0u,
	      "self-assignment releases exactly once without pool rejection");

	group("RefcountWidth");
	endpoint.receive_adu(wire);
	Packet original = endpoint.pop_packet();
	constexpr std::size_t copies_count = 70000u;
	{
		std::vector<Packet> copies;
		copies.reserve(copies_count);
		for (std::size_t i = 0u; i < copies_count; ++i) {
			copies.emplace_back(original);
		}
		check(endpoint.storage().rx_available() == 2u &&
		      equal(original.data(), data),
		      "70,000 shared handles do not wrap the uint32_t reference count");
	}
	check(endpoint.storage().rx_available() == 2u && original,
	      "the original remains live after every copy is released");
	original.reset();
	check(endpoint.storage().rx_available() == 3u,
	      "the last of more than 65,535 references frees exactly once");

	group("EmptyData");
	const auto minimum = make_adu(0x00u, 0xA7u);
	endpoint.receive_adu(minimum);
	Packet empty_data = endpoint.pop_packet();
	check(empty_data && empty_data.address() == 0u && empty_data.function() == 0xA7u,
	      "minimum four-byte ADU preserves broadcast/custom metadata");
	check(empty_data.data().empty() && empty_data.size() == 0u &&
	      empty_data.pdu().size() == 1u && empty_data.adu().size() == 4u,
	      "empty function data has consistent data/PDU/ADU views");
	empty_data.reset();

	group("MaximumAdu");
	std::vector<uint8_t> maximum_data(modbus::max_data_size);
	for (std::size_t i = 0; i < maximum_data.size(); ++i) {
		maximum_data[i] = static_cast<uint8_t>(i);
	}
	const auto maximum = make_adu(0xF7u, 0x64u, maximum_data);
	check(maximum.size() == 256u, "252 function-data bytes form an exact 256-byte ADU");
	endpoint.receive_adu(maximum);
	Packet full = endpoint.pop_packet();
	check(full && full.adu().size() == 256u && full.size() == 252u &&
	      equal(full.data(), maximum_data),
	      "maximum ADU remains fully addressable through every Packet view");
	full.reset();

	group("RetainedBackpressure");
	std::vector<Packet> retained;
	for (uint8_t i = 0; i < 3u; ++i) {
		const auto adu = make_adu(static_cast<uint8_t>(i + 1u), 0x03u);
		endpoint.receive_adu(adu);
		retained.push_back(endpoint.pop_packet());
	}
	check(endpoint.storage().rx_available() == 0u,
	      "retained shared handles consume the configured RX quota");
	endpoint.receive_adu(minimum);
	check(!endpoint.pop_packet() && endpoint.stats().rx.allocation_failure == 1u,
	      "pool exhaustion is backpressure, not a dangling Packet");
	retained.clear();
	check(endpoint.storage().rx_available() == 3u,
	      "releasing retained handles restores reception capacity");

	return finish();
}
