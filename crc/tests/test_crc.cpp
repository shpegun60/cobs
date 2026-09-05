/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "crc/Crc.h"
#include "Test.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <type_traits>
#include <vector>

using namespace crc_test;

namespace {

constexpr std::array<uint8_t, 9u> check_input{
	'1', '2', '3', '4', '5', '6', '7', '8', '9'};

static_assert(crc::Crc8Bitwise{}.calculate(check_input) == 0xF4u);
static_assert(crc::Crc8Table{}.calculate(check_input) == 0xF4u);
static_assert(crc::Crc16Bitwise{}.calculate(check_input) == 0x4B37u);
static_assert(crc::Crc16Table{}.calculate(check_input) == 0x4B37u);
static_assert(crc::Crc32Bitwise{}.calculate(check_input) == 0xCBF43926u);
static_assert(crc::Crc32Table{}.calculate(check_input) == 0xCBF43926u);
static_assert(crc::Crc64Bitwise{}.calculate(check_input) ==
	UINT64_C(0x6C40DF5F0B497347));
static_assert(crc::Crc64Table{}.calculate(check_input) ==
	UINT64_C(0x6C40DF5F0B497347));

static_assert(std::is_empty_v<crc::Crc8Bitwise>);
static_assert(std::is_empty_v<crc::Crc8Table>);
static_assert(std::is_empty_v<crc::Crc16Bitwise>);
static_assert(std::is_empty_v<crc::Crc16Table>);
static_assert(std::is_empty_v<crc::Crc32Bitwise>);
static_assert(std::is_empty_v<crc::Crc32Table>);
static_assert(std::is_empty_v<crc::Crc64Bitwise>);
static_assert(std::is_empty_v<crc::Crc64Table>);
static_assert(std::is_empty_v<crc::NoCrc>);

struct Sum16 final : crc::Codec<uint16_t, 2u, std::endian::little> {
	explicit Sum16(unsigned& count) noexcept : calls(&count) {}

	unsigned* calls = nullptr;

	[[nodiscard]] uint16_t calculate(
			const std::span<const uint8_t> bytes) noexcept
	{
		++*calls;
		uint16_t value = 0u;
		for (const uint8_t byte : bytes) {
			value = static_cast<uint16_t>(
				value + static_cast<uint16_t>(byte));
		}
		return value;
	}
};

struct MissingCalculate final : crc::Codec<uint16_t> {};
struct ThrowingCalculate final : crc::Codec<uint16_t> {
	uint16_t calculate(std::span<const uint8_t>);
};
struct MissingCodec final {
	using value_type = uint16_t;
	static constexpr std::size_t wire_size = 2u;
	uint16_t calculate(std::span<const uint8_t>) noexcept;
};

static_assert(crc::Policy<Sum16>);
static_assert(!crc::Policy<MissingCalculate>);
static_assert(!crc::Policy<ThrowingCalculate>);
static_assert(!crc::Policy<MissingCodec>);

/* The register type is one of four fixed-width unsigned types, nothing else. */
static_assert(crc::Value<uint8_t> && crc::Value<uint16_t> &&
              crc::Value<uint32_t> && crc::Value<uint64_t>);
static_assert(!crc::Value<bool>);
static_assert(!crc::Value<char16_t> && !crc::Value<char32_t>);
static_assert(!crc::Value<int> && !crc::Value<std::byte>);

/*
 * A policy whose default constructor may throw is a valid Policy, but the
 * unconditionally-noexcept default-constructing helpers must refuse it
 * instead of accepting it and terminating on the first throw.
 */
struct ThrowyCtor final : crc::Codec<uint16_t> {
	ThrowyCtor() noexcept(false) {}
	uint16_t calculate(std::span<const uint8_t>) noexcept { return 0u; }
};

template<class PolicyT>
concept AcceptedByDefaultConstructingHelpers =
	requires(const std::span<const uint8_t> bytes) {
		crc::calculate<PolicyT>(bytes);
		crc::verify<PolicyT>(bytes);
	};

static_assert(crc::Policy<ThrowyCtor>);
static_assert(!AcceptedByDefaultConstructingHelpers<ThrowyCtor>);
static_assert(AcceptedByDefaultConstructingHelpers<crc::Crc16Bitwise>);

/*
 * Catalogue models beyond the four defaults, chosen to cover what the
 * defaults cannot: a non-reflected 16- and 32-bit path, non-zero Initial and
 * XorOut in non-reflected mode, a reflected 8- and 64-bit path, and an
 * ASYMMETRIC initial value in reflected mode. Parameters are in the register
 * domain (see the note above the aliases in Crc.h): reflected models take the
 * reflected polynomial AND the reflected initial value.
 */
using CcittFalse = crc::Crc16<crc::Bitwise, 0x1021u, 0xFFFFu, 0x0000u, false, std::endian::big>;
using CcittFalseTable = crc::Crc16<crc::Table, 0x1021u, 0xFFFFu, 0x0000u, false, std::endian::big>;
using Genibus = crc::Crc16<crc::Bitwise, 0x1021u, 0xFFFFu, 0xFFFFu, false, std::endian::big>;
using GenibusTable = crc::Crc16<crc::Table, 0x1021u, 0xFFFFu, 0xFFFFu, false, std::endian::big>;
using Bzip2 = crc::Crc32<crc::Bitwise, 0x04C11DB7u, 0xFFFFFFFFu, 0xFFFFFFFFu, false, std::endian::big>;
using Bzip2Table = crc::Crc32<crc::Table, 0x04C11DB7u, 0xFFFFFFFFu, 0xFFFFFFFFu, false, std::endian::big>;
using Iscsi = crc::Crc32<crc::Bitwise, 0x82F63B78u, 0xFFFFFFFFu, 0xFFFFFFFFu, true, std::endian::little>;
using IscsiTable = crc::Crc32<crc::Table, 0x82F63B78u, 0xFFFFFFFFu, 0xFFFFFFFFu, true, std::endian::little>;
using MaximDow = crc::Crc8<crc::Bitwise, 0x8Cu, 0x00u, 0x00u, true, std::endian::big>;
using MaximDowTable = crc::Crc8<crc::Table, 0x8Cu, 0x00u, 0x00u, true, std::endian::big>;
using Xz = crc::Crc64<crc::Bitwise, UINT64_C(0xC96C5795D7870F42),
	UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF), true, std::endian::little>;
using XzTable = crc::Crc64<crc::Table, UINT64_C(0xC96C5795D7870F42),
	UINT64_C(0xFFFFFFFFFFFFFFFF), UINT64_C(0xFFFFFFFFFFFFFFFF), true, std::endian::little>;
// CRC-16/ISO-IEC-14443-3-A: catalogue init 0xC6C6, register-domain 0x6363.
using CrcA = crc::Crc16<crc::Bitwise, 0x8408u, 0x6363u, 0x0000u, true, std::endian::little>;
using CrcATable = crc::Crc16<crc::Table, 0x8408u, 0x6363u, 0x0000u, true, std::endian::little>;
using CrcAWrongInit = crc::Crc16<crc::Bitwise, 0x8408u, 0xC6C6u, 0x0000u, true, std::endian::little>;

static_assert(CcittFalse{}.calculate(check_input) == 0x29B1u);
static_assert(CcittFalseTable{}.calculate(check_input) == 0x29B1u);
static_assert(Genibus{}.calculate(check_input) == 0xD64Eu);
static_assert(GenibusTable{}.calculate(check_input) == 0xD64Eu);
static_assert(Bzip2{}.calculate(check_input) == 0xFC891918u);
static_assert(Bzip2Table{}.calculate(check_input) == 0xFC891918u);
static_assert(Iscsi{}.calculate(check_input) == 0xE3069283u);
static_assert(IscsiTable{}.calculate(check_input) == 0xE3069283u);
static_assert(MaximDow{}.calculate(check_input) == 0xA1u);
static_assert(MaximDowTable{}.calculate(check_input) == 0xA1u);
static_assert(Xz{}.calculate(check_input) == UINT64_C(0x995DC9BBDF1939FA));
static_assert(XzTable{}.calculate(check_input) == UINT64_C(0x995DC9BBDF1939FA));
static_assert(CrcA{}.calculate(check_input) == 0xBF05u);
static_assert(CrcATable{}.calculate(check_input) == 0xBF05u);
static_assert(CrcAWrongInit{}.calculate(check_input) == 0x1480u,
	"the catalogue init copied verbatim into a reflected engine is a different model");

template<class ValueT>
[[nodiscard]] ValueT independent_reflected(
		const std::span<const uint8_t> bytes,
		const ValueT polynomial,
		const ValueT initial,
		const ValueT xor_out)
{
	ValueT value = initial;
	for (const uint8_t byte : bytes) {
		value = static_cast<ValueT>(value ^ static_cast<ValueT>(byte));
		for (unsigned bit = 0u; bit < 8u; ++bit) {
			const bool low = (value & ValueT{1u}) != ValueT{0u};
			value = static_cast<ValueT>(value >> 1u);
			if (low) {
				value = static_cast<ValueT>(value ^ polynomial);
			}
		}
	}
	return static_cast<ValueT>(value ^ xor_out);
}

template<class ValueT>
[[nodiscard]] ValueT independent_forward(
		const std::span<const uint8_t> bytes,
		const ValueT polynomial,
		const ValueT initial,
		const ValueT xor_out)
{
	constexpr unsigned width = sizeof(ValueT) * 8u;
	constexpr ValueT high = static_cast<ValueT>(ValueT{1u} << (width - 1u));
	ValueT value = initial;
	for (const uint8_t byte : bytes) {
		value = static_cast<ValueT>(value ^ static_cast<ValueT>(
			static_cast<ValueT>(byte) << (width - 8u)));
		for (unsigned bit = 0u; bit < 8u; ++bit) {
			const bool top = (value & high) != ValueT{0u};
			value = static_cast<ValueT>(value << 1u);
			if (top) {
				value = static_cast<ValueT>(value ^ polynomial);
			}
		}
	}
	return static_cast<ValueT>(value ^ xor_out);
}

template<class PolicyT>
[[nodiscard]] std::vector<uint8_t> append_check(
		PolicyT& policy,
		const std::span<const uint8_t> bytes)
{
	std::vector<uint8_t> frame(bytes.begin(), bytes.end());
	frame.resize(bytes.size() + PolicyT::wire_size);
	const typename PolicyT::value_type value = policy.calculate(bytes);
	policy.store(frame.data() + bytes.size(), value);
	return frame;
}

} // namespace

int main()
{
	group("KnownModels");
	check(crc::Crc8Bitwise{}.calculate(check_input) == 0xF4u &&
	      crc::Crc8Table{}.calculate(check_input) == 0xF4u,
	      "CRC8 bitwise/table match CRC-8/SMBUS check value");
	check(crc::Crc16Bitwise{}.calculate(check_input) == 0x4B37u &&
	      crc::Crc16Table{}.calculate(check_input) == 0x4B37u,
	      "CRC16 bitwise/table match CRC-16/MODBUS check value");
	check(crc::Crc32Bitwise{}.calculate(check_input) == 0xCBF43926u &&
	      crc::Crc32Table{}.calculate(check_input) == 0xCBF43926u,
	      "CRC32 bitwise/table match CRC-32/ISO-HDLC check value");
	check(crc::Crc64Bitwise{}.calculate(check_input) ==
	          UINT64_C(0x6C40DF5F0B497347) &&
	      crc::Crc64Table{}.calculate(check_input) ==
	          UINT64_C(0x6C40DF5F0B497347),
	      "CRC64 bitwise/table match CRC-64/ECMA-182 check value");

	group("CatalogueModels");
	check(CcittFalse{}.calculate(check_input) == 0x29B1u &&
	      CcittFalseTable{}.calculate(check_input) == 0x29B1u,
	      "CRC-16/CCITT-FALSE: non-reflected 16-bit path with Initial != 0");
	check(Genibus{}.calculate(check_input) == 0xD64Eu &&
	      GenibusTable{}.calculate(check_input) == 0xD64Eu,
	      "CRC-16/GENIBUS: non-reflected with XorOut != 0");
	check(Bzip2{}.calculate(check_input) == 0xFC891918u &&
	      Bzip2Table{}.calculate(check_input) == 0xFC891918u,
	      "CRC-32/BZIP2: non-reflected 32-bit path");
	check(Iscsi{}.calculate(check_input) == 0xE3069283u &&
	      IscsiTable{}.calculate(check_input) == 0xE3069283u,
	      "CRC-32/ISCSI: reflected with a second polynomial");
	check(MaximDow{}.calculate(check_input) == 0xA1u &&
	      MaximDowTable{}.calculate(check_input) == 0xA1u,
	      "CRC-8/MAXIM-DOW: reflected 8-bit path");
	check(Xz{}.calculate(check_input) == UINT64_C(0x995DC9BBDF1939FA) &&
	      XzTable{}.calculate(check_input) == UINT64_C(0x995DC9BBDF1939FA),
	      "CRC-64/XZ: reflected 64-bit path, all-ones Initial and XorOut");
	check(CrcA{}.calculate(check_input) == 0xBF05u &&
	      CrcATable{}.calculate(check_input) == 0xBF05u &&
	      CrcAWrongInit{}.calculate(check_input) == 0x1480u,
	      "CRC-16/ISO-IEC-14443-3-A: reflected Initial 0x6363 gives 0xBF05, verbatim 0xC6C6 does not");

	group("WireCodecs");
	std::array<uint8_t, 8u> bytes{};
	crc::Codec<uint32_t, 4u, std::endian::little>::store(
		bytes.data(), 0x12345678u);
	check(bytes[0] == 0x78u && bytes[1] == 0x56u &&
	      bytes[2] == 0x34u && bytes[3] == 0x12u &&
	      crc::Codec<uint32_t, 4u, std::endian::little>::load(bytes.data()) ==
	          0x12345678u,
	      "little-endian codec is explicit and round-trips");
	crc::Codec<uint64_t, 8u, std::endian::big>::store(
		bytes.data(), UINT64_C(0x0123456789ABCDEF));
	check(bytes[0] == 0x01u && bytes[7] == 0xEFu &&
	      crc::Codec<uint64_t, 8u, std::endian::big>::load(bytes.data()) ==
	          UINT64_C(0x0123456789ABCDEF),
	      "big-endian codec is explicit and round-trips");
	crc::Codec<uint32_t, 3u, std::endian::little>::store(
		bytes.data(), 0xA1B2C3D4u);
	check(bytes[0] == 0xD4u && bytes[1] == 0xC3u && bytes[2] == 0xB2u &&
	      crc::Codec<uint32_t, 3u, std::endian::little>::load(bytes.data()) ==
	          0x00B2C3D4u,
	      "codec permits an intentional truncated wire width");

	group("RandomOracle");
	std::mt19937 random{0x43524331u};
	std::uniform_int_distribution<int> length_distribution(0, 512);
	std::uniform_int_distribution<int> byte_distribution(0, 255);
	bool all_match = true;
	for (unsigned iteration = 0u; iteration < 20000u; ++iteration) {
		std::vector<uint8_t> input(
			static_cast<std::size_t>(length_distribution(random)));
		for (uint8_t& byte : input) {
			byte = static_cast<uint8_t>(byte_distribution(random));
		}
		const uint8_t expected8 = independent_forward<uint8_t>(
			input, 0x07u, 0x00u, 0x00u);
		const uint16_t expected16 = independent_reflected<uint16_t>(
			input, 0xA001u, 0xFFFFu, 0x0000u);
		const uint32_t expected32 = independent_reflected<uint32_t>(
			input, 0xEDB88320u, 0xFFFFFFFFu, 0xFFFFFFFFu);
		const uint64_t expected64 = independent_forward<uint64_t>(
			input, UINT64_C(0x42F0E1EBA9EA3693), UINT64_C(0), UINT64_C(0));
		all_match = all_match &&
			crc::Crc8Bitwise{}.calculate(input) == expected8 &&
			crc::Crc8Table{}.calculate(input) == expected8 &&
			crc::Crc16Bitwise{}.calculate(input) == expected16 &&
			crc::Crc16Table{}.calculate(input) == expected16 &&
			crc::Crc32Bitwise{}.calculate(input) == expected32 &&
			crc::Crc32Table{}.calculate(input) == expected32 &&
			crc::Crc64Bitwise{}.calculate(input) == expected64 &&
			crc::Crc64Table{}.calculate(input) == expected64;
	}
	check(all_match,
	      "all eight built-ins match independent oracles on 20,000 inputs");

	group("PolicyAndVerify");
	unsigned calls = 0u;
	Sum16 sum{calls};
	const auto sum_frame = append_check(sum, check_input);
	check(crc::verify(sum_frame, sum) && calls == 2u,
	      "stateful custom checksum owns calculate/store/load semantics");
	auto damaged = sum_frame;
	damaged.front() ^= 0x80u;
	check(!crc::verify(damaged, sum),
	      "generic verification compares only the selected policy result");

	crc::NoCrc no_crc;
	const auto no_crc_frame = append_check(no_crc, check_input);
	check(no_crc_frame.size() == check_input.size() &&
	      crc::verify(no_crc_frame, no_crc) &&
	      crc::verify(std::span<const uint8_t>{}, no_crc),
	      "NoCrc adds zero bytes and accepts even an empty framed body");
	check(crc::NoCrc::wire_size == 0u &&
	      crc::Crc8Bitwise::wire_size == 1u &&
	      crc::Crc16Bitwise::wire_size == 2u &&
	      crc::Crc32Bitwise::wire_size == 4u &&
	      crc::Crc64Bitwise::wire_size == 8u,
	      "every policy publishes its compile-time wire width");

	return finish();
}
