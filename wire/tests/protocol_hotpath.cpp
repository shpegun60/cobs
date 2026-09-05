/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Compile-only wrappers exposing complete protocol primitives to objdump. */

#include "cobs/Format.h"
#include "modbus/Pdu.h"
#include "modbus/rtu/Crc.h"

#include <cstddef>
#include <cstdint>
#include <span>

#if defined(__GNUC__)
#define WIRE_NOINLINE __attribute__((noinline, used))
#else
#define WIRE_NOINLINE
#endif

extern "C" WIRE_NOINLINE void cobs_store_length16(
		uint8_t* const destination,
		const std::size_t value) noexcept
{
	cobs::Format<256u>::store_length(destination, value);
}

extern "C" WIRE_NOINLINE std::size_t cobs_load_length16(
		const uint8_t* const source) noexcept
{
	return cobs::Format<256u>::load_length(source);
}

extern "C" WIRE_NOINLINE void modbus_crc_store(
		uint8_t* const destination,
		const uint16_t value) noexcept
{
	modbus::rtu::crc::Bitwise::store(destination, value);
}

extern "C" WIRE_NOINLINE uint16_t modbus_crc_load(
		const uint8_t* const source) noexcept
{
	return modbus::rtu::crc::Bitwise::load(source);
}

extern "C" WIRE_NOINLINE uint32_t modbus_read_be32_exact(
		const uint8_t* const source) noexcept
{
	std::size_t offset = 0u;
	uint32_t value = 0u;
	(void)modbus::read_be(
		std::span<const uint8_t>{source, sizeof(value)}, offset, value);
	return value;
}

extern "C" WIRE_NOINLINE uint32_t modbus_read_le32_exact(
		const uint8_t* const source) noexcept
{
	std::size_t offset = 0u;
	uint32_t value = 0u;
	(void)modbus::read_le(
		std::span<const uint8_t>{source, sizeof(value)}, offset, value);
	return value;
}

extern "C" WIRE_NOINLINE uint16_t modbus_crc_calculate(
		const uint8_t* const source,
		const std::size_t size) noexcept
{
	return modbus::rtu::crc::calculate(
		std::span<const uint8_t>{source, size});
}

extern "C" WIRE_NOINLINE uint16_t modbus_crc_calculate_table(
		const uint8_t* const source,
		const std::size_t size) noexcept
{
	return modbus::rtu::crc::calculate<modbus::rtu::crc::Table>(
		std::span<const uint8_t>{source, size});
}

#undef WIRE_NOINLINE
