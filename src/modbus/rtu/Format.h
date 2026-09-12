/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * RTU wire geometry, in two layers:
 *
 *     Layout<CrcSize, MaxData>  geometry only: sizes and offsets, keyed on the
 *                               trailer WIDTH and the function-data limit
 *     Format<Crc, MaxData>      the wire contract: the integrity policy TYPE
 *                               plus that data limit; exposes its Layout
 *
 * Packet, Message and Receiver depend on the Layout, never on the Format, so
 * CRC policies of equal width — Crc16Bitwise and Crc16Table — share one
 * instantiation of each, and an Endpoint's storage geometry is identical for
 * both. Choosing the table implementation changes the calculator and nothing
 * else.
 *
 * The Format is what a user names on the endpoint and what two peers must
 * agree on: `modbus::rtu::Format<>` is standard Modbus RTU (CRC-16/MODBUS,
 * 252 data bytes, 256-byte ADU). Address, function and CRC are added to the
 * data limit automatically. Actual ADUs above 256 or different checksum semantics are
 * private RTU-like exchanges. A smaller ceiling only limits local capacity.
 * A custom calculator type cannot prove or disprove Modbus compatibility;
 * choosing and configuring it is the application's responsibility.
 */

#ifndef MODBUS_RTU_FORMAT_H_
#define MODBUS_RTU_FORMAT_H_

#include "../../crc/Crc.h"
#include "../Types.h"
#include "RtuLimits.h"

#include <concepts>
#include <cstddef>
#include <cstdint>

namespace modbus::rtu {

template<std::size_t CrcSize, std::size_t MaxData = modbus::max_data_size>
struct Layout final {
	static constexpr std::size_t address_size = 1u;
	static constexpr std::size_t function_size = 1u;
	static constexpr std::size_t crc_size = CrcSize;
	static constexpr std::size_t adu_prefix_size =
		address_size + function_size;

	// Validate user-supplied counts before additions, even for SIZE_MAX. Zero
	// data is legal: the library still allocates address + function + trailer.
	// RxBlock::adu_size remains uint16_t, bounding private ADUs to 65535 bytes.
	static_assert(CrcSize <= UINT16_MAX - adu_prefix_size,
		"CRC wire_size must leave room for RTU address and function");
	static_assert(MaxData <= (CrcSize <= UINT16_MAX - adu_prefix_size
		? UINT16_MAX - adu_prefix_size - CrcSize : 0u),
		"RTU data plus address, function and CRC must fit the 65535-byte ADU");

	static constexpr std::size_t adu_overhead =
		adu_prefix_size + crc_size;
	static constexpr std::size_t pdu_envelope_size =
		address_size + crc_size;
	static constexpr std::size_t min_adu_size = adu_overhead;
	static constexpr std::size_t max_data_size = MaxData;
	static constexpr std::size_t max_adu_size = max_data_size + adu_overhead;
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

template<class CrcT = ::crc::Crc16Bitwise, std::size_t MaxData = modbus::max_data_size>
struct Format final {
	static_assert(::crc::Policy<CrcT>,
		"RTU Format CRC must satisfy crc::Policy");

	using Crc = CrcT;
	using Layout = modbus::rtu::Layout<CrcT::wire_size, MaxData>;

	static constexpr std::size_t max_adu_size = Layout::max_adu_size;
	static constexpr std::size_t crc_size = Layout::crc_size;
	static constexpr std::size_t max_data_size = Layout::max_data_size;
	static constexpr std::size_t max_pdu_size = Layout::max_pdu_size;

};

using DefaultFormat = Format<>;

static_assert(DefaultFormat::crc_size == 2u);
static_assert(DefaultFormat::max_adu_size == 256u);
static_assert(DefaultFormat::Layout::min_adu_size == 4u);
static_assert(DefaultFormat::max_data_size == 252u);
static_assert(DefaultFormat::max_pdu_size == 253u);

} // namespace modbus::rtu

#endif /* MODBUS_RTU_FORMAT_H_ */
