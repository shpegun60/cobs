/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "wire/Scalar.h"

#include <bit>
#include <cstdint>

#if defined(__GNUC__)
#define WIRE_NOINLINE __attribute__((noinline, used))
#else
#define WIRE_NOINLINE
#endif

extern "C" WIRE_NOINLINE void wire_store_be16(
		uint8_t* const destination,
		const uint16_t value) noexcept
{
	wire::detail::store_ordered<std::endian::big>(destination, value);
}

extern "C" WIRE_NOINLINE void wire_store_le16(
		uint8_t* const destination,
		const uint16_t value) noexcept
{
	wire::detail::store_ordered<std::endian::little>(destination, value);
}

extern "C" WIRE_NOINLINE void wire_store_be32(
		uint8_t* const destination,
		const uint32_t value) noexcept
{
	wire::detail::store_ordered<std::endian::big>(destination, value);
}

extern "C" WIRE_NOINLINE void wire_store_le32(
		uint8_t* const destination,
		const uint32_t value) noexcept
{
	wire::detail::store_ordered<std::endian::little>(destination, value);
}

extern "C" WIRE_NOINLINE void wire_store_be64(
		uint8_t* const destination,
		const uint64_t value) noexcept
{
	wire::detail::store_ordered<std::endian::big>(destination, value);
}

extern "C" WIRE_NOINLINE void wire_store_le64(
		uint8_t* const destination,
		const uint64_t value) noexcept
{
	wire::detail::store_ordered<std::endian::little>(destination, value);
}

extern "C" WIRE_NOINLINE void wire_store_be_float(
		uint8_t* const destination,
		const float value) noexcept
{
	wire::detail::store_ordered<std::endian::big>(destination, value);
}

extern "C" WIRE_NOINLINE void wire_store_le_float(
		uint8_t* const destination,
		const float value) noexcept
{
	wire::detail::store_ordered<std::endian::little>(destination, value);
}

extern "C" WIRE_NOINLINE uint16_t wire_load_be16(
		const uint8_t* const source) noexcept
{
	return wire::detail::load_ordered<std::endian::big, uint16_t>(source);
}

extern "C" WIRE_NOINLINE uint16_t wire_load_le16(
		const uint8_t* const source) noexcept
{
	return wire::detail::load_ordered<std::endian::little, uint16_t>(source);
}

extern "C" WIRE_NOINLINE uint32_t wire_load_be32(
		const uint8_t* const source) noexcept
{
	return wire::detail::load_ordered<std::endian::big, uint32_t>(source);
}

extern "C" WIRE_NOINLINE uint32_t wire_load_le32(
		const uint8_t* const source) noexcept
{
	return wire::detail::load_ordered<std::endian::little, uint32_t>(source);
}

extern "C" WIRE_NOINLINE uint64_t wire_load_be64(
		const uint8_t* const source) noexcept
{
	return wire::detail::load_ordered<std::endian::big, uint64_t>(source);
}

extern "C" WIRE_NOINLINE uint64_t wire_load_le64(
		const uint8_t* const source) noexcept
{
	return wire::detail::load_ordered<std::endian::little, uint64_t>(source);
}

extern "C" WIRE_NOINLINE float wire_load_be_float(
		const uint8_t* const source) noexcept
{
	return wire::detail::load_ordered<std::endian::big, float>(source);
}

extern "C" WIRE_NOINLINE float wire_load_le_float(
		const uint8_t* const source) noexcept
{
	return wire::detail::load_ordered<std::endian::little, float>(source);
}
