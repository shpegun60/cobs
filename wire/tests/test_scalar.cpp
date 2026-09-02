/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Independent host verification for the shared native/BE/LE scalar codec. */

#include "wire/Scalar.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace {

enum class Code : uint16_t { Value = 0xA1B2u };
enum class BoolCode : bool { No = false, Yes = true };

struct Record {
	uint8_t byte;
	uint32_t word;
};

int failures = 0;

void check(const bool condition, const char* const description)
{
	if (!condition) {
		++failures;
		std::printf("FAIL  %s\n", description);
	}
}

template<class U>
	requires std::is_unsigned_v<U>
bool exact_ordered_bytes(
		const std::span<const uint8_t> bytes,
		const U value,
		const std::endian order)
{
	for (std::size_t index = 0u; index < sizeof(U); ++index) {
		const std::size_t shift = order == std::endian::little
			? index * 8u
			: (sizeof(U) - 1u - index) * 8u;
		if (bytes[index] != static_cast<uint8_t>(value >> shift)) {
			return false;
		}
	}
	return true;
}

void test_type_contract()
{
	static_assert(wire::Scalar<uint8_t> && wire::Scalar<int64_t>);
	static_assert(wire::Scalar<float> && wire::Scalar<double>);
	static_assert(wire::Scalar<Code> && wire::Scalar<std::byte>);
	static_assert(!wire::Scalar<bool> && !wire::Scalar<BoolCode>);
	static_assert(!wire::Scalar<Record> && !wire::Scalar<uint8_t*>);
	static_assert(!wire::Scalar<volatile uint32_t>);

	static_assert(wire::EndianScalar<uint8_t>);
	static_assert(wire::EndianScalar<uint16_t>);
	static_assert(wire::EndianScalar<uint32_t>);
	static_assert(wire::EndianScalar<uint64_t>);
	static_assert(wire::EndianScalar<float> && wire::EndianScalar<double>);
	static_assert(wire::EndianScalar<Code>);
	static_assert(!wire::EndianScalar<long double>);
	static_assert(!wire::EndianScalar<Record>);
	check(true, "scalar domains are compile-time constrained");
}

void test_all_u16()
{
	std::array<uint8_t, 5> bytes{};
	bool correct = true;
	for (uint32_t raw = 0u; raw <= UINT16_MAX; ++raw) {
		const uint16_t value = static_cast<uint16_t>(raw);

		bytes.fill(0xA5u);
		wire::detail::store_ordered<std::endian::big>(bytes.data() + 1u, value);
		correct = correct && bytes.front() == 0xA5u && bytes.back() == 0xA5u &&
			exact_ordered_bytes<uint16_t>(
				std::span<const uint8_t>{bytes}.subspan(1u, 2u), value,
				std::endian::big) &&
			wire::detail::load_ordered<std::endian::big, uint16_t>(
				bytes.data() + 1u) == value;

		bytes.fill(0x5Au);
		wire::detail::store_ordered<std::endian::little>(bytes.data() + 1u, value);
		correct = correct && bytes.front() == 0x5Au && bytes.back() == 0x5Au &&
			exact_ordered_bytes<uint16_t>(
				std::span<const uint8_t>{bytes}.subspan(1u, 2u), value,
				std::endian::little) &&
			wire::detail::load_ordered<std::endian::little, uint16_t>(
				bytes.data() + 1u) == value;
	}
	check(correct,
	      "all 65,536 uint16 bit patterns round-trip in both orders unaligned");
}

uint64_t next_random(uint64_t& state)
{
	state ^= state << 13u;
	state ^= state >> 7u;
	state ^= state << 17u;
	return state;
}

void test_random_wide_scalars()
{
	std::array<uint8_t, 24> bytes{};
	uint64_t state = UINT64_C(0x8A5CD789635D2DFF);
	bool correct = true;
	for (std::size_t iteration = 0u; iteration < 250000u; ++iteration) {
		const uint64_t value64 = next_random(state);
		const uint32_t value32 = static_cast<uint32_t>(next_random(state));
		const std::size_t offset = 1u + (iteration % 7u);

		bytes.fill(0xC3u);
		wire::detail::store_ordered<std::endian::big>(
			bytes.data() + offset, value64);
		correct = correct &&
			exact_ordered_bytes<uint64_t>(
				std::span<const uint8_t>{bytes}.subspan(offset, 8u), value64,
				std::endian::big) &&
			wire::detail::load_ordered<std::endian::big, uint64_t>(
				bytes.data() + offset) == value64;

		wire::detail::store_ordered<std::endian::little>(
			bytes.data() + offset + 1u, value32);
		correct = correct &&
			exact_ordered_bytes<uint32_t>(
				std::span<const uint8_t>{bytes}.subspan(offset + 1u, 4u), value32,
				std::endian::little) &&
			wire::detail::load_ordered<std::endian::little, uint32_t>(
				bytes.data() + offset + 1u) == value32;
	}
	check(correct,
	      "250,000 uint32/uint64 patterns survive every unaligned offset");
}

void test_native_and_floating()
{
	/* Keep the loaded temporary named: taking the address of a return is ill-formed. */
	const auto native_ok = []<class T>(const T& value, const std::size_t offset) {
		std::array<uint8_t, 40> bytes{};
		bytes.fill(0x9Du);
		wire::detail::store_native(bytes.data() + offset, value);
		const T loaded = wire::detail::load_native<T>(bytes.data() + offset);
		return bytes[offset - 1u] == 0x9Du &&
		       bytes[offset + sizeof(T)] == 0x9Du &&
		       std::memcmp(bytes.data() + offset, &value, sizeof(T)) == 0 &&
		       std::memcmp(&loaded, &value, sizeof(T)) == 0;
	};

	check(native_ok(uint16_t{0x1234u}, 1u) &&
	      native_ok(uint32_t{0x89ABCDEFu}, 3u) &&
	      native_ok(UINT64_C(0x0102030405060708), 5u) &&
	      native_ok(Code::Value, 7u),
	      "native scalar representation round-trips at unaligned addresses");

	static_assert(std::numeric_limits<float>::is_iec559 && sizeof(float) == 4u);
	static_assert(std::numeric_limits<double>::is_iec559 && sizeof(double) == 8u);
	constexpr std::array<uint32_t, 6> float_bits{
		0x00000000u, 0x80000000u, 0x3FC00000u,
		0xFF800000u, 0x7FC12345u, 0x00000001u};
	constexpr std::array<uint64_t, 6> double_bits{
		UINT64_C(0x0000000000000000), UINT64_C(0x8000000000000000),
		UINT64_C(0x3FF8000000000000), UINT64_C(0xFFF0000000000000),
		UINT64_C(0x7FF8123456789ABC), UINT64_C(0x0000000000000001)};
	std::array<uint8_t, 18> bytes{};
	bool correct = true;
	for (const uint32_t bits : float_bits) {
		const float value = std::bit_cast<float>(bits);
		wire::detail::store_ordered<std::endian::big>(bytes.data() + 1u, value);
		const float loaded = wire::detail::load_ordered<std::endian::big, float>(
			bytes.data() + 1u);
		correct = correct && std::bit_cast<uint32_t>(loaded) == bits &&
			exact_ordered_bytes<uint32_t>(
				std::span<const uint8_t>{bytes}.subspan(1u, 4u), bits,
				std::endian::big);
	}
	for (const uint64_t bits : double_bits) {
		const double value = std::bit_cast<double>(bits);
		wire::detail::store_ordered<std::endian::little>(bytes.data() + 3u, value);
		const double loaded = wire::detail::load_ordered<std::endian::little, double>(
			bytes.data() + 3u);
		correct = correct && std::bit_cast<uint64_t>(loaded) == bits &&
			exact_ordered_bytes<uint64_t>(
				std::span<const uint8_t>{bytes}.subspan(3u, 8u), bits,
				std::endian::little);
	}
	check(correct,
	      "float/double preserve zero, infinity, NaN and subnormal bit patterns");
}

void test_ordered_spans()
{
	constexpr std::array<uint32_t, 3> values{
		0x01020304u, 0xA1B2C3D4u, 0x00000000u};
	std::array<uint8_t, 16> bytes{};
	bytes.fill(0xE7u);
	wire::detail::store_ordered<std::endian::big>(
		bytes.data() + 1u, std::span<const uint32_t>{values});
	constexpr std::array<uint8_t, 12> expected{
		0x01u, 0x02u, 0x03u, 0x04u,
		0xA1u, 0xB2u, 0xC3u, 0xD4u,
		0x00u, 0x00u, 0x00u, 0x00u};
	check(bytes.front() == 0xE7u && bytes[13] == 0xE7u &&
	      std::equal(expected.begin(), expected.end(), bytes.begin() + 1),
	      "ordered spans serialize each element independently");
}

} // namespace

int main()
{
	test_type_contract();
	test_all_u16();
	test_random_wide_scalars();
	test_native_and_floating();
	test_ordered_spans();
	std::printf("wire scalar: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
