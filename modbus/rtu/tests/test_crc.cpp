/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "crc/Crc.h"
#include "Test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <type_traits>
#include <vector>

using namespace modbus_test;

namespace {

struct AdaptiveCrc final : ::crc::Codec<uint16_t, 2u, std::endian::little> {
	AdaptiveCrc(unsigned& bitwise, unsigned& table) noexcept
		: bitwise_calls(&bitwise), table_calls(&table) {}

	unsigned* bitwise_calls = nullptr;
	unsigned* table_calls = nullptr;

	[[nodiscard]] uint16_t calculate(
			const std::span<const uint8_t> bytes) noexcept
	{
		if (bytes.size() < 32u) {
			++*bitwise_calls;
			return ::crc::Crc16Bitwise{}.calculate(bytes);
		}
		++*table_calls;
		return ::crc::Crc16Table{}.calculate(bytes);
	}
};

struct FakeHardware final : ::crc::Codec<uint16_t, 2u, std::endian::little> {
	explicit FakeHardware(unsigned& count) noexcept : calls(&count) {}

	unsigned* calls = nullptr;

	[[nodiscard]] uint16_t calculate(
			const std::span<const uint8_t> bytes) noexcept
	{
		++*calls;
		return ::crc::Crc16Bitwise{}.calculate(bytes);
	}
};

struct MissingCalculate final {};
struct ThrowingCalculate final {
	using value_type = uint16_t;
	static constexpr std::size_t wire_size = 2u;
	uint16_t calculate(std::span<const uint8_t>);
	void store(uint8_t*, uint16_t) noexcept;
	uint16_t load(const uint8_t*) noexcept;
};
struct WrongResult final {
	using value_type = uint16_t;
	static constexpr std::size_t wire_size = 2u;
	uint32_t calculate(std::span<const uint8_t>) noexcept;
	void store(uint8_t*, uint16_t) noexcept;
	uint16_t load(const uint8_t*) noexcept;
};

static_assert(::crc::Policy<::crc::Crc16Bitwise>);
static_assert(::crc::Policy<::crc::Crc16Table>);
static_assert(::crc::Policy<AdaptiveCrc>);
static_assert(::crc::Policy<FakeHardware>);
static_assert(!::crc::Policy<MissingCalculate>);
static_assert(!::crc::Policy<ThrowingCalculate>);
static_assert(!::crc::Policy<WrongResult>);
static_assert(std::is_empty_v<::crc::Crc16Bitwise>);
static_assert(std::is_empty_v<::crc::Crc16Table>);

// Independent 16-entry nibble oracle. It shares neither the eight-step
// bitwise loop nor the built-in Table policy's 256-entry lookup geometry.
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
	constexpr uint16_t known = ::crc::calculate<::crc::Crc16Bitwise>(request);
	constexpr uint16_t known_bitwise =
		::crc::calculate<::crc::Crc16Bitwise>(request);
	constexpr uint16_t known_table =
		::crc::calculate<::crc::Crc16Table>(request);
	static_assert(known == 0xCDC5u);
	static_assert(known_bitwise == known && known_table == known);
	constexpr auto compile_time_wire_crc = [] {
		std::array<uint8_t, ::crc::Crc16Bitwise::wire_size> bytes{};
		::crc::Crc16Bitwise::store(bytes.data(), 0xCDC5u);
		return bytes;
	}();
	static_assert(compile_time_wire_crc[0] == 0xC5u &&
	              compile_time_wire_crc[1] == 0xCDu);
	static_assert(::crc::Crc16Bitwise::load(
		compile_time_wire_crc.data()) == 0xCDC5u);
	check(known == 0xCDC5u, "01 03 00 00 00 0A produces wire CRC C5 CD");
	const auto known_adu = make_adu(0x01u, 0x03u,
		std::span<const uint8_t>{request}.subspan(2u));
	check(known_adu[known_adu.size() - 2u] == 0xC5u &&
	      known_adu[known_adu.size() - 1u] == 0xCDu,
	      "CRC is serialized low byte first");
	check(::crc::verify<::crc::Crc16Bitwise>(known_adu), "known complete ADU verifies");
	check(::crc::verify<::crc::Crc16Table>(known_adu),
	      "table implementation verifies the same complete ADU");
	check(!::crc::verify<::crc::Crc16Bitwise>({}), "an absent CRC never verifies");

	group("Corruption");
	for (std::size_t byte = 0; byte < known_adu.size(); ++byte) {
		for (unsigned bit = 0; bit < 8u; ++bit) {
			auto damaged = known_adu;
			damaged[byte] ^= static_cast<uint8_t>(1u << bit);
			check(!::crc::verify<::crc::Crc16Bitwise>(damaged),
			      "every single-bit corruption is rejected");
			check(!::crc::verify<::crc::Crc16Table>(damaged),
			      "table CRC rejects every single-bit corruption");
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
		const uint16_t expected = reference_crc(bytes);
		all_match = all_match &&
			::crc::calculate<::crc::Crc16Bitwise>(bytes) == expected &&
			::crc::calculate<::crc::Crc16Bitwise>(bytes) == expected &&
			::crc::calculate<::crc::Crc16Table>(bytes) == expected;
	}
	check(all_match,
	      "default, Bitwise and Table match an independent oracle on 20,000 inputs");

	group("ExhaustiveTwoByteDomain");
	std::array<uint8_t, 2> pair{};
	bool every_pair_matches = true;
	for (unsigned first = 0u; first <= 0xFFu; ++first) {
		pair[0] = static_cast<uint8_t>(first);
		for (unsigned second = 0u; second <= 0xFFu; ++second) {
			pair[1] = static_cast<uint8_t>(second);
			const uint16_t expected = reference_crc(pair);
			every_pair_matches = every_pair_matches &&
				::crc::calculate<::crc::Crc16Bitwise>(pair) == expected &&
				::crc::calculate<::crc::Crc16Table>(pair) == expected;
		}
	}
	check(every_pair_matches,
	      "all 65,536 two-byte inputs match the independent oracle");

	group("CustomPolicies");
	unsigned bitwise_calls = 0u;
	unsigned table_calls = 0u;
	AdaptiveCrc adaptive{bitwise_calls, table_calls};
	unsigned hardware_calls = 0u;
	FakeHardware hardware{hardware_calls};
	std::array<uint8_t, 31u> short_input{};
	std::array<uint8_t, 32u> long_input{};
	for (std::size_t index = 0u; index < long_input.size(); ++index) {
		long_input[index] = static_cast<uint8_t>(index * 17u + 3u);
		if (index < short_input.size()) {
			short_input[index] = long_input[index];
		}
	}
	check(::crc::calculate(short_input, adaptive) ==
	          ::crc::calculate<::crc::Crc16Bitwise>(short_input) &&
	      ::crc::calculate(long_input, adaptive) ==
	          ::crc::calculate<::crc::Crc16Bitwise>(long_input) &&
	      bitwise_calls == 1u && table_calls == 1u,
	      "adaptive policy may select an implementation by length without changing CRC");
	check(::crc::calculate(long_input, hardware) ==
	          ::crc::calculate<::crc::Crc16Bitwise>(long_input) &&
	      hardware_calls == 1u,
	      "stateful fake-hardware policy uses its handle and preserves CRC semantics");

	return finish();
}
