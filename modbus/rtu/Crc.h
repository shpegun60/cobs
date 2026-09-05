/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef MODBUS_RTU_CRC_H_
#define MODBUS_RTU_CRC_H_

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace modbus::rtu::crc {

inline constexpr std::size_t wire_size = 2u;

namespace detail {

[[nodiscard]] constexpr uint16_t update_bitwise(
		uint16_t value, const uint8_t byte) noexcept
{
	value = static_cast<uint16_t>(
		value ^ static_cast<uint16_t>(byte));
	for (unsigned bit = 0u; bit < 8u; ++bit) {
		value = (value & 1u) != 0u
			? static_cast<uint16_t>((value >> 1u) ^ 0xA001u)
			: static_cast<uint16_t>(value >> 1u);
	}
	return value;
}

[[nodiscard]] constexpr std::array<uint16_t, 256u> make_table() noexcept
{
	std::array<uint16_t, 256u> result{};
	for (std::size_t byte = 0u; byte < result.size(); ++byte) {
		result[byte] = update_bitwise(0u, static_cast<uint8_t>(byte));
	}
	return result;
}

inline constexpr auto table = make_table();

} // namespace detail

/* Portable, table-free CRC-16/MODBUS implementation. */
struct Bitwise final {
	[[nodiscard]] constexpr uint16_t calculate(
			const std::span<const uint8_t> bytes) const noexcept
	{
		uint16_t value = 0xFFFFu;
		for (const uint8_t byte : bytes) {
			value = detail::update_bitwise(value, byte);
		}
		return value;
	}
};

/* CRC-16/MODBUS implementation using one immutable 256-entry lookup table. */
struct Table final {
	[[nodiscard]] constexpr uint16_t calculate(
			const std::span<const uint8_t> bytes) const noexcept
	{
		uint16_t value = 0xFFFFu;
		for (const uint8_t byte : bytes) {
			const std::size_t index = static_cast<std::size_t>(
				(value ^ static_cast<uint16_t>(byte)) & 0x00FFu);
			value = static_cast<uint16_t>(
				(value >> 8u) ^ detail::table[index]);
		}
		return value;
	}
};

/*
 * Structural contract only. Built-ins calculate CRC-16/MODBUS; a custom
 * policy may intentionally implement any other 16-bit check value.
 */
template<class T>
concept Calculator = requires(
		T& calculator, const std::span<const uint8_t> bytes) {
	{ calculator.calculate(bytes) } noexcept -> std::same_as<uint16_t>;
};

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

template<Calculator CalculatorT>
[[nodiscard]] constexpr uint16_t calculate(
		const std::span<const uint8_t> bytes,
		CalculatorT& calculator) noexcept
{
	return calculator.calculate(bytes);
}

// Backward-compatible default: CRC-16/MODBUS without a lookup table.
[[nodiscard]] constexpr uint16_t calculate(
		const std::span<const uint8_t> bytes) noexcept
{
	return Bitwise{}.calculate(bytes);
}

template<Calculator CalculatorT>
	requires std::default_initializable<CalculatorT>
[[nodiscard]] constexpr uint16_t calculate(
		const std::span<const uint8_t> bytes) noexcept
{
	CalculatorT calculator{};
	return calculator.calculate(bytes);
}

template<Calculator CalculatorT>
[[nodiscard]] constexpr bool verify(
		const std::span<const uint8_t> adu,
		CalculatorT& calculator) noexcept
{
	if (adu.size() < wire_size) {
		return false;
	}
	const std::size_t payload_size = adu.size() - wire_size;
	const uint16_t expected = calculate(
		adu.first(payload_size), calculator);
	const uint16_t received = load(adu.data() + payload_size);
	return expected == received;
}

[[nodiscard]] constexpr bool verify(
		const std::span<const uint8_t> adu) noexcept
{
	Bitwise calculator{};
	return verify(adu, calculator);
}

template<Calculator CalculatorT>
	requires std::default_initializable<CalculatorT>
[[nodiscard]] constexpr bool verify(
		const std::span<const uint8_t> adu) noexcept
{
	CalculatorT calculator{};
	return verify(adu, calculator);
}

} // namespace modbus::rtu::crc

#endif /* MODBUS_RTU_CRC_H_ */
