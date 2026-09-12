/* Author: shpegun60; SPDX-License-Identifier: MIT */
#ifndef MODBUS_TCP_FORMAT_H_
#define MODBUS_TCP_FORMAT_H_

#include "../Types.h"
#include "../../crc/Crc.h"

namespace modbus::tcp {

// The limit counts application data only, exactly like Message::size() and
// Packet::data(). MBAP, function and optional CRC are added automatically.
// Geometry depends on trailer WIDTH, never on the calculator type.
template<std::size_t CrcSize, std::size_t MaxData = modbus::max_data_size>
struct Layout final {
	static constexpr std::size_t length_prefix_size = 6u;
	static constexpr std::size_t mbap_size = 7u;
	static constexpr std::size_t function_size = 1u;
	static constexpr std::size_t adu_prefix_size = mbap_size + function_size;
	static constexpr std::size_t crc_size = CrcSize;
	// Length counts unit + function + data + CRC. Validate BEFORE adding any
	// user-supplied sizes, including SIZE_MAX. Keep the subtraction safe even
	// while diagnosing an invalid CRC width.
	static_assert(CrcSize <= UINT16_MAX - 2u,
		"CRC wire_size must leave room for TCP unit and function in MBAP Length");
	static_assert(MaxData <= (CrcSize <= UINT16_MAX - 2u ? UINT16_MAX - 2u - CrcSize : 0u),
		"TCP data plus CRC, unit and function must fit the 65535-byte MBAP Length");
	static constexpr std::size_t adu_overhead = adu_prefix_size + crc_size;
	static constexpr std::size_t min_adu_size = adu_overhead;
	static constexpr std::size_t pdu_envelope_size = mbap_size + crc_size;
	static constexpr std::size_t max_data_size = MaxData;
	static constexpr std::size_t max_adu_size = adu_overhead + max_data_size;
	static constexpr std::size_t max_pdu_size = function_size + max_data_size;

	// The caller has checked data_size <= max_data_size before this addition.
	[[nodiscard]] static constexpr std::size_t adu_size_for_data(
		const std::size_t data_size) noexcept { return adu_overhead + data_size; }
	[[nodiscard]] static constexpr std::size_t data_capacity_for_adu(
		const std::size_t bytes) noexcept
	{
		return bytes >= adu_overhead ? bytes - adu_overhead : 0u;
	}
};

// NoCrc is STANDARD Modbus TCP. Any trailer is an explicitly private format;
// both peers agree on its width, encoding and calculation (no autodetection).
template<class CrcT = ::crc::NoCrc, std::size_t MaxData = modbus::max_data_size>
struct Format final {
	static_assert(::crc::Policy<CrcT>, "TCP Format CRC must satisfy crc::Policy");
	using Crc = CrcT;
	using Layout = modbus::tcp::Layout<CrcT::wire_size, MaxData>;
	static constexpr std::size_t max_adu_size = Layout::max_adu_size;
	static constexpr std::size_t crc_size = Layout::crc_size;
	static constexpr std::size_t max_data_size = Layout::max_data_size;
	static constexpr std::size_t max_pdu_size = Layout::max_pdu_size;
};

using DefaultFormat = Format<>;
static_assert(DefaultFormat::max_adu_size == 260u);
static_assert(DefaultFormat::max_data_size == 252u);
static_assert(DefaultFormat::crc_size == 0u);
static_assert(Format<::crc::Crc16Bitwise>::max_data_size == 252u);
static_assert(Format<::crc::Crc16Bitwise>::max_adu_size == 262u);

} // namespace modbus::tcp
#endif
