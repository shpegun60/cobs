/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Compile-only probe proving which CRC implementation Endpoint instantiates. */

#include "modbus/rtu/Rtu.h"

#include <cstddef>
#include <cstdint>
#include <span>

#if defined(MODBUS_CRC_TABLE_PROBE)
using ProbeEndpoint = modbus::rtu::Endpoint<
	modbus::rtu::Heap, modbus::rtu::crc::Table>;
#else
using ProbeEndpoint = modbus::rtu::Endpoint<>;
#endif

#if defined(__GNUC__)
#define MODBUS_NOINLINE __attribute__((noinline, used))
#else
#define MODBUS_NOINLINE
#endif

extern "C" MODBUS_NOINLINE void modbus_endpoint_crc_receive_probe(
		ProbeEndpoint& endpoint,
		const uint8_t* const bytes,
		const std::size_t size) noexcept
{
	endpoint.receive_adu(std::span<const uint8_t>{bytes, size});
}

#undef MODBUS_NOINLINE
