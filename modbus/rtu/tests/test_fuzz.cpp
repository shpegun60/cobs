/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"
#include "Test.h"

#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

using namespace modbus_test;

namespace {

uint16_t reference_crc(const std::span<const uint8_t> bytes)
{
	uint16_t value = 0xFFFFu;
	for (const uint8_t byte : bytes) {
		value = static_cast<uint16_t>(
			value ^ static_cast<uint16_t>(byte));
		for (unsigned bit = 0; bit < 8u; ++bit) {
			const bool low = (value & 1u) != 0u;
			value = static_cast<uint16_t>(value >> 1u);
			if (low) {
				value ^= 0xA001u;
			}
		}
	}
	return value;
}

bool reference_valid(const std::span<const uint8_t> candidate)
{
	if (candidate.size() < 4u || candidate.size() > 256u) {
		return false;
	}
	const std::size_t body = candidate.size() - 2u;
	const uint16_t expected = reference_crc(candidate.first(body));
	const uint16_t received = static_cast<uint16_t>(
		candidate[body] | (static_cast<uint16_t>(candidate[body + 1u]) << 8u));
	return expected == received;
}

} // namespace

int main()
{
	group("RandomCandidates");
	modbus::rtu::Endpoint<> endpoint;
	std::mt19937 random{0x52545531u};
	std::uniform_int_distribution<int> length_distribution(0, 300);
	std::uniform_int_distribution<int> byte_distribution(0, 255);
	bool correct = true;
	std::size_t expected_packets = 0u;
	std::size_t actual_packets = 0u;

	for (unsigned iteration = 0; iteration < 100000u; ++iteration) {
		std::vector<uint8_t> candidate(
			static_cast<std::size_t>(length_distribution(random)));
		for (uint8_t& byte : candidate) {
			byte = static_cast<uint8_t>(byte_distribution(random));
		}
		const bool expected = reference_valid(candidate);
		expected_packets += expected ? 1u : 0u;
		endpoint.receive_adu(candidate);
		auto packet = endpoint.pop_packet();
		actual_packets += packet ? 1u : 0u;
		correct = correct && static_cast<bool>(packet) == expected;
		if (packet) {
			correct = correct && packet.adu().size() >= 4u &&
			          packet.adu().size() <= 256u &&
			          equal(packet.adu(), candidate) &&
			          packet.pdu().size() == packet.data().size() + 1u;
		}
	}

	check(correct && actual_packets == expected_packets,
	      "100,000 random candidates publish exactly the independently valid ADUs");
	check(endpoint.stats().rx.candidates == 100000u,
	      "every fuzz input is accounted for once");

	group("ConstructedValidFrames");
	bool all_round_trip = true;
	for (std::size_t data_size = 0u; data_size <= modbus::max_data_size; ++data_size) {
		std::vector<uint8_t> data(data_size);
		for (std::size_t i = 0u; i < data.size(); ++i) {
			data[i] = static_cast<uint8_t>((i * 37u + data_size) & 0xFFu);
		}
		const auto adu = make_adu(0xF7u, 0xA7u, data);
		endpoint.receive_adu(adu);
		auto packet = endpoint.pop_packet();
		all_round_trip = all_round_trip && packet &&
		                 packet.address() == 0xF7u &&
		                 packet.function() == 0xA7u &&
		                 equal(packet.data(), data) && equal(packet.adu(), adu);
	}
	check(all_round_trip,
	      "every legal data length 0..252 round-trips with arbitrary custom bytes");

	return finish();
}
