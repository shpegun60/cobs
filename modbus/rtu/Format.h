/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* RTU wire geometry derived entirely from the selected CRC policy. */

#ifndef MODBUS_RTU_FORMAT_H_
#define MODBUS_RTU_FORMAT_H_

#include "Crc.h"
#include "RtuLimits.h"

#include <cstddef>

namespace modbus::rtu {

template<std::size_t CrcSize = modbus::rtu::crc::Bitwise::wire_size>
struct Format final {
	static constexpr std::size_t max_adu_size = modbus::rtu::max_adu_size;
	static constexpr std::size_t address_size = 1u;
	static constexpr std::size_t function_size = 1u;
	static constexpr std::size_t crc_size = CrcSize;
	static constexpr std::size_t adu_prefix_size =
		address_size + function_size;

	static_assert(crc_size <= max_adu_size - adu_prefix_size,
		"CRC wire_size leaves no room for RTU address and function");

	static constexpr std::size_t adu_overhead =
		adu_prefix_size + crc_size;
	static constexpr std::size_t pdu_envelope_size =
		address_size + crc_size;
	static constexpr std::size_t min_adu_size = adu_overhead;
	static constexpr std::size_t max_data_size =
		max_adu_size - adu_overhead;
	static constexpr std::size_t max_pdu_size =
		function_size + max_data_size;

	[[nodiscard]] static constexpr std::size_t adu_size_for_data(
			const std::size_t data_size) noexcept
	{
		return adu_overhead + data_size;
	}

	[[nodiscard]] static constexpr std::size_t data_capacity_for_adu(
			const std::size_t adu_capacity) noexcept
	{
		return adu_capacity >= adu_overhead
			? adu_capacity - adu_overhead
			: 0u;
	}
};

using DefaultFormat = Format<>;

static_assert(DefaultFormat::crc_size == 2u);
static_assert(DefaultFormat::min_adu_size == 4u);
static_assert(DefaultFormat::max_data_size == 252u);
static_assert(DefaultFormat::max_pdu_size == 253u);

} // namespace modbus::rtu

#endif /* MODBUS_RTU_FORMAT_H_ */
