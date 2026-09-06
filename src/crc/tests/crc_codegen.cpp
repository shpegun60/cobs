/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Compile-only probe for table ownership and conditional emission. */

#include "crc/Crc.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

static_assert(std::is_empty_v<crc::Crc8Table>);
static_assert(std::is_empty_v<crc::Crc16Table>);
static_assert(std::is_empty_v<crc::Crc32Table>);
static_assert(std::is_empty_v<crc::Crc64Table>);

#if defined(__GNUC__)
#define CRC_NOINLINE __attribute__((noinline, used))
#else
#define CRC_NOINLINE
#endif

#define CRC_CODEC_PROBE(Bits, Type, Order, Endian) \
	extern "C" CRC_NOINLINE void crc_store_##Order##Bits( \
			uint8_t* const destination, const Type value) noexcept \
	{ \
		crc::Codec<Type, sizeof(Type), Endian>::store(destination, value); \
	} \
	extern "C" CRC_NOINLINE Type crc_load_##Order##Bits( \
			const uint8_t* const source) noexcept \
	{ \
		return crc::Codec<Type, sizeof(Type), Endian>::load(source); \
	}
CRC_CODEC_PROBE(8, uint8_t, le, std::endian::little)
CRC_CODEC_PROBE(16, uint16_t, le, std::endian::little)
CRC_CODEC_PROBE(32, uint32_t, le, std::endian::little)
CRC_CODEC_PROBE(64, uint64_t, le, std::endian::little)
CRC_CODEC_PROBE(8, uint8_t, be, std::endian::big)
CRC_CODEC_PROBE(16, uint16_t, be, std::endian::big)
CRC_CODEC_PROBE(32, uint32_t, be, std::endian::big)
CRC_CODEC_PROBE(64, uint64_t, be, std::endian::big)

#undef CRC_CODEC_PROBE

#if defined(CRC_TABLE_8_PROBE)
using ProbePolicy = crc::Crc8Table;
#elif defined(CRC_TABLE_16_PROBE)
using ProbePolicy = crc::Crc16Table;
#elif defined(CRC_TABLE_32_PROBE)
using ProbePolicy = crc::Crc32Table;
#elif defined(CRC_TABLE_64_PROBE)
using ProbePolicy = crc::Crc64Table;
#elif defined(CRC_BITWISE_8_PROBE)
using ProbePolicy = crc::Crc8Bitwise;
#elif defined(CRC_BITWISE_16_PROBE)
using ProbePolicy = crc::Crc16Bitwise;
#elif defined(CRC_BITWISE_32_PROBE)
using ProbePolicy = crc::Crc32Bitwise;
#elif defined(CRC_BITWISE_64_PROBE)
using ProbePolicy = crc::Crc64Bitwise;
#elif defined(CRC_NO_CRC_PROBE)
using ProbePolicy = crc::NoCrc;
#else
using ProbePolicy = crc::Crc16Bitwise;
#endif

extern "C" CRC_NOINLINE uint64_t crc_policy_calculate_probe(
		const uint8_t* const bytes,
		const std::size_t size) noexcept
{
	ProbePolicy policy{};
	return static_cast<uint64_t>(
		policy.calculate(std::span<const uint8_t>{bytes, size}));
}

extern "C" CRC_NOINLINE bool crc_no_crc_verify_probe(
		const uint8_t* const bytes,
		const std::size_t size) noexcept
{
	crc::NoCrc policy{};
	return crc::verify(
		std::span<const uint8_t>{bytes, size}, policy);
}

#undef CRC_NOINLINE
