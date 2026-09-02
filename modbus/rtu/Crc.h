/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef MODBUS_RTU_CRC_H_
#define MODBUS_RTU_CRC_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace modbus::rtu::crc {

inline constexpr std::size_t wire_size = 2u;

/* Modbus RTU always transmits the CRC low byte first, regardless of CPU. */
constexpr void store(uint8_t* const destination, const uint16_t value) noexcept
{
	destination[0] = static_cast<uint8_t>(value & 0xFFu);
	destination[1] = static_cast<uint8_t>(value >> 8u);
}

[[nodiscard]] constexpr uint16_t load(
		const uint8_t* const source) noexcept
{
	return static_cast<uint16_t>(
		static_cast<uint16_t>(source[0]) |
		(static_cast<uint16_t>(source[1]) << 8u));
}

// MODBUS CRC-16: initial 0xFFFF, reflected polynomial 0xA001.
[[nodiscard]] constexpr uint16_t calculate(
		const std::span<const uint8_t> bytes) noexcept
{
	uint16_t value = 0xFFFFu;
	for (const uint8_t byte : bytes) {
		value = static_cast<uint16_t>(
			value ^ static_cast<uint16_t>(byte));
		for (unsigned bit = 0; bit < 8u; ++bit) {
			value = (value & 1u) != 0u
				? static_cast<uint16_t>((value >> 1u) ^ 0xA001u)
				: static_cast<uint16_t>(value >> 1u);
		}
	}
	return value;
}

[[nodiscard]] constexpr bool verify(
		const std::span<const uint8_t> adu) noexcept
{
	if (adu.size() < wire_size) {
		return false;
	}
	const std::size_t payload_size = adu.size() - wire_size;
	const uint16_t expected = calculate(adu.first(payload_size));
	const uint16_t received = load(adu.data() + payload_size);
	return expected == received;
}

} // namespace modbus::rtu::crc

#endif /* MODBUS_RTU_CRC_H_ */
