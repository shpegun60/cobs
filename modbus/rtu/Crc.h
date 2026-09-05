/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Modbus names for the protocol-independent CRC policy library. */

#ifndef MODBUS_RTU_CRC_H_
#define MODBUS_RTU_CRC_H_

#include "../../crc/Crc.h"

#include <concepts>
#include <cstdint>
#include <span>
#include <type_traits>

namespace modbus::rtu::crc {

/* Standard Modbus RTU defaults retained as concise compatibility names. */
using Bitwise = ::crc::Crc16Bitwise;
using Table = ::crc::Crc16Table;
using NoCrc = ::crc::NoCrc;

template<class T>
concept Policy = ::crc::Policy<T>;

template<Policy PolicyT>
[[nodiscard]] constexpr typename PolicyT::value_type calculate(
		const std::span<const uint8_t> bytes,
		PolicyT& policy) noexcept
{
	return ::crc::calculate(bytes, policy);
}

[[nodiscard]] constexpr uint16_t calculate(
		const std::span<const uint8_t> bytes) noexcept
{
	return Bitwise{}.calculate(bytes);
}

template<Policy PolicyT>
	requires std::is_nothrow_default_constructible_v<PolicyT>
[[nodiscard]] constexpr typename PolicyT::value_type calculate(
		const std::span<const uint8_t> bytes) noexcept
{
	return ::crc::calculate<PolicyT>(bytes);
}

template<Policy PolicyT>
[[nodiscard]] constexpr bool verify(
		const std::span<const uint8_t> frame,
		PolicyT& policy) noexcept
{
	return ::crc::verify(frame, policy);
}

[[nodiscard]] constexpr bool verify(
		const std::span<const uint8_t> frame) noexcept
{
	return ::crc::verify<Bitwise>(frame);
}

template<Policy PolicyT>
	requires std::is_nothrow_default_constructible_v<PolicyT>
[[nodiscard]] constexpr bool verify(
		const std::span<const uint8_t> frame) noexcept
{
	return ::crc::verify<PolicyT>(frame);
}

} // namespace modbus::rtu::crc

#endif /* MODBUS_RTU_CRC_H_ */
