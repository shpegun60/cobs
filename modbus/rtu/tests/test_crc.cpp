/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Crc.h"
#include "Test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

using namespace modbus_test;

namespace {

// Independent table-driven oracle. The production implementation is
// deliberately tableless, so agreement does not merely execute the same loop.
uint16_t reference_crc(const std::span<const uint8_t> bytes)
{
	static constexpr std::array<uint16_t, 16> table{
		0x0000u, 0xCC01u, 0xD801u, 0x1400u,
		0xF001u, 0x3C00u, 0x2800u, 0xE401u,
		0xA001u, 0x6C00u, 0x7800u, 0xB401u,
		0x5000u, 0x9C01u, 0x8801u, 0x4400u,
	};
	uint16_t crc = 0xFFFFu;
	for (const uint8_t byte : bytes) {
		crc = static_cast<uint16_t>(
			crc ^ static_cast<uint16_t>(byte));
		crc = static_cast<uint16_t>((crc >> 4u) ^ table[crc & 0x0Fu]);
		crc = static_cast<uint16_t>((crc >> 4u) ^ table[crc & 0x0Fu]);
	}
	return crc;
}

} // namespace

int main()
{
	group("KnownVector");
	constexpr std::array<uint8_t, 6> request{0x01u, 0x03u, 0x00u, 0x00u, 0x00u, 0x0Au};
	constexpr uint16_t known = modbus::rtu::crc::calculate(request);
	static_assert(known == 0xCDC5u);
	constexpr auto compile_time_wire_crc = [] {
		std::array<uint8_t, modbus::rtu::crc::wire_size> bytes{};
		modbus::rtu::crc::store(bytes.data(), 0xCDC5u);
		return bytes;
	}();
	static_assert(compile_time_wire_crc[0] == 0xC5u &&
	              compile_time_wire_crc[1] == 0xCDu);
	static_assert(modbus::rtu::crc::load(compile_time_wire_crc.data()) == 0xCDC5u);
	check(known == 0xCDC5u, "01 03 00 00 00 0A produces wire CRC C5 CD");
	const auto known_adu = make_adu(0x01u, 0x03u,
		std::span<const uint8_t>{request}.subspan(2u));
	check(known_adu[known_adu.size() - 2u] == 0xC5u &&
	      known_adu[known_adu.size() - 1u] == 0xCDu,
	      "CRC is serialized low byte first");
	check(modbus::rtu::crc::verify(known_adu), "known complete ADU verifies");
	check(!modbus::rtu::crc::verify({}), "an absent CRC never verifies");

	group("Corruption");
	for (std::size_t byte = 0; byte < known_adu.size(); ++byte) {
		for (unsigned bit = 0; bit < 8u; ++bit) {
			auto damaged = known_adu;
			damaged[byte] ^= static_cast<uint8_t>(1u << bit);
			check(!modbus::rtu::crc::verify(damaged),
			      "every single-bit corruption is rejected");
		}
	}

	group("RandomOracle");
	std::mt19937 random{0x4D4F4442u};
	std::uniform_int_distribution<int> length_distribution(0, 256);
	std::uniform_int_distribution<int> byte_distribution(0, 255);
	bool all_match = true;
	for (unsigned iteration = 0; iteration < 20000u; ++iteration) {
		std::vector<uint8_t> bytes(
			static_cast<std::size_t>(length_distribution(random)));
		for (uint8_t& byte : bytes) {
			byte = static_cast<uint8_t>(byte_distribution(random));
		}
		all_match = all_match &&
			modbus::rtu::crc::calculate(bytes) == reference_crc(bytes);
	}
	check(all_match,
	      "20,000 random inputs match an independent nibble-table oracle");

	return finish();
}
