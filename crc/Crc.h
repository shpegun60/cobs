/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Header-only, protocol-independent CRC policies.
 *
 * A policy owns three independent compile-time decisions:
 *
 *   calculate(bytes)  algorithm and optional runtime state
 *   wire_size         number of trailer bytes carried by a protocol
 *   store/load        exact byte representation of that trailer
 *
 * Protocol libraries depend only on the Policy concept below. They do not
 * assume a result type, polynomial, byte order, or even that a trailer exists.
 */

#ifndef CRC_CRC_H_
#define CRC_CRC_H_

#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

namespace crc {

/* Algorithm selection tags. They contain no state and allocate no memory. */
struct Bitwise final {};
struct Table final {};

/*
 * The result widths this library models: exactly the four fixed-width
 * unsigned types. std::unsigned_integral would also admit bool (a Codec<bool>
 * compiles but load(0x02) -> store() yields 0x01, not a byte-exact round
 * trip), the char types and platform-sized integers, none of which anyone
 * means as a CRC register. Naming the four types keeps the accident out.
 */
template<class T>
concept Value = std::same_as<T, uint8_t> || std::same_as<T, uint16_t> ||
                std::same_as<T, uint32_t> || std::same_as<T, uint64_t>;

/*
 * Reusable integer wire codec for custom policies.
 *
 * A hardware calculator or private checksum normally derives from Codec and
 * implements only calculate(). WireSize may be smaller than ValueT when a
 * deliberately truncated check value is required, including zero bytes.
 */
template<
	Value ValueT,
	std::size_t WireSize = sizeof(ValueT),
	std::endian WireOrder = std::endian::little>
class Codec {
	static_assert(WireSize <= sizeof(ValueT),
		"CRC wire_size cannot exceed the integer result width");
	static_assert(WireOrder == std::endian::little ||
	              WireOrder == std::endian::big,
		"CRC wire order must be explicitly little-endian or big-endian");

public:
	using value_type = ValueT;
	static constexpr std::size_t wire_size = WireSize;
	static constexpr std::endian wire_order = WireOrder;

	static constexpr void store(
			uint8_t* const destination,
			const value_type value) noexcept
	{
		if constexpr (wire_size != 0u) {
			store_impl(destination, value,
			           std::make_index_sequence<wire_size>{});
		}
	}

	[[nodiscard]] static constexpr value_type load(
			const uint8_t* const source) noexcept
	{
		if constexpr (wire_size != 0u) {
			return load_impl(source, std::make_index_sequence<wire_size>{});
		} else {
			return 0u;
		}
	}

private:
	template<std::size_t Index>
	static constexpr std::size_t byte_shift =
		(wire_order == std::endian::little
			? Index
			: wire_size - 1u - Index) * 8u;

	template<std::size_t... Indices>
	static constexpr void store_impl(
			uint8_t* const destination,
			const value_type value,
			std::index_sequence<Indices...>) noexcept
	{
		((destination[Indices] = static_cast<uint8_t>(
			value >> byte_shift<Indices>)), ...);
	}

	template<std::size_t... Indices>
	[[nodiscard]] static constexpr value_type load_impl(
			const uint8_t* const source,
			std::index_sequence<Indices...>) noexcept
	{
		value_type value = 0u;
		((value = static_cast<value_type>(value |
			(static_cast<value_type>(source[Indices]) <<
			 byte_shift<Indices>))), ...);
		return value;
	}
};

/*
 * Structural policy contract. Semantics are intentionally not prescribed:
 * a custom policy may calculate a standard CRC, use hardware, return a sum,
 * or provide no trailer. The protocol uses exactly what the policy declares.
 */
template<class T>
concept Policy = requires {
	typename T::value_type;
	typename std::integral_constant<std::size_t, T::wire_size>;
} && std::equality_comparable<typename T::value_type> && requires(
		T& policy,
		const std::span<const uint8_t> bytes,
		uint8_t* const destination,
		const uint8_t* const source,
		const typename T::value_type value) {
	{ policy.calculate(bytes) } noexcept -> std::same_as<typename T::value_type>;
	{ policy.store(destination, value) } noexcept -> std::same_as<void>;
	{ policy.load(source) } noexcept -> std::same_as<typename T::value_type>;
};

namespace detail {

template<class MethodT>
concept Method = std::same_as<MethodT, crc::Bitwise> ||
	std::same_as<MethodT, crc::Table>;

template<
	Value ValueT,
	ValueT Polynomial,
	bool Reflected>
[[nodiscard]] constexpr ValueT update_bitwise(
		ValueT value,
		const uint8_t byte) noexcept
{
	constexpr unsigned width = std::numeric_limits<ValueT>::digits;
	static_assert(width >= 8u);

	if constexpr (Reflected) {
		value = static_cast<ValueT>(
			value ^ static_cast<ValueT>(byte));
		for (unsigned bit = 0u; bit < 8u; ++bit) {
			const bool set = (value & ValueT{1u}) != ValueT{0u};
			value = static_cast<ValueT>(value >> 1u);
			if (set) {
				value = static_cast<ValueT>(value ^ Polynomial);
			}
		}
	} else {
		constexpr unsigned shift = width - 8u;
		constexpr ValueT high_bit = static_cast<ValueT>(
			ValueT{1u} << (width - 1u));
		value = static_cast<ValueT>(
			value ^ static_cast<ValueT>(static_cast<ValueT>(byte) << shift));
		for (unsigned bit = 0u; bit < 8u; ++bit) {
			const bool set = (value & high_bit) != ValueT{0u};
			value = static_cast<ValueT>(value << 1u);
			if (set) {
				value = static_cast<ValueT>(value ^ Polynomial);
			}
		}
	}
	return value;
}

template<
	class MethodT,
	Value ValueT,
	ValueT Polynomial,
	ValueT Initial,
	ValueT XorOut,
	bool Reflected,
	std::endian WireOrder>
	requires Method<MethodT>
class Engine;

template<
	Value ValueT,
	ValueT Polynomial,
	ValueT Initial,
	ValueT XorOut,
	bool Reflected,
	std::endian WireOrder>
class Engine<
		crc::Bitwise,
		ValueT,
		Polynomial,
		Initial,
		XorOut,
		Reflected,
		WireOrder> : public crc::Codec<ValueT, sizeof(ValueT), WireOrder> {
public:
	[[nodiscard]] constexpr ValueT calculate(
			const std::span<const uint8_t> bytes) const noexcept
	{
		ValueT value = Initial;
		for (const uint8_t byte : bytes) {
			value = update_bitwise<ValueT, Polynomial, Reflected>(value, byte);
		}
		return static_cast<ValueT>(value ^ XorOut);
	}
};

template<
	Value ValueT,
	ValueT Polynomial,
	ValueT Initial,
	ValueT XorOut,
	bool Reflected,
	std::endian WireOrder>
class Engine<
		crc::Table,
		ValueT,
		Polynomial,
		Initial,
		XorOut,
		Reflected,
		WireOrder> : public crc::Codec<ValueT, sizeof(ValueT), WireOrder> {
public:
	[[nodiscard]] constexpr ValueT calculate(
			const std::span<const uint8_t> bytes) const noexcept
	{
		constexpr unsigned width = std::numeric_limits<ValueT>::digits;
		ValueT value = Initial;
		for (const uint8_t byte : bytes) {
			if constexpr (Reflected) {
				const std::size_t index = static_cast<std::size_t>(
					(value ^ static_cast<ValueT>(byte)) & ValueT{0xFFu});
				value = static_cast<ValueT>(
					(value >> 8u) ^ lookup_[index]);
			} else {
				constexpr unsigned shift = width - 8u;
				const std::size_t index = static_cast<std::size_t>(
					((value >> shift) ^ static_cast<ValueT>(byte)) &
					ValueT{0xFFu});
				value = static_cast<ValueT>(
					static_cast<ValueT>(value << 8u) ^ lookup_[index]);
			}
		}
		return static_cast<ValueT>(value ^ XorOut);
	}

private:
	[[nodiscard]] static constexpr std::array<ValueT, 256u>
	make_lookup() noexcept
	{
		std::array<ValueT, 256u> result{};
		for (std::size_t byte = 0u; byte < result.size(); ++byte) {
			result[byte] = update_bitwise<ValueT, Polynomial, Reflected>(
				ValueT{0u}, static_cast<uint8_t>(byte));
		}
		return result;
	}

	/*
	 * The lookup belongs to this exact Table class specialization. Merely
	 * including this header or using any Bitwise policy does not instantiate or
	 * emit it. Runtime use of one specialization emits one immutable COMDAT in
	 * read-only program memory and never places it inside a policy object.
	 */
	inline static constexpr std::array<ValueT, 256u> lookup_ = make_lookup();
};

} // namespace detail

/*
 * Parameter domain for the four alias templates below.
 *
 * Polynomial, Initial and XorOut are the REGISTER values of the selected
 * implementation, not the catalogue (Rocksoft) model values. For a
 * non-reflected model they coincide. For a reflected model (refin = refout =
 * true) the engine keeps the register bit-reversed, so:
 *
 *     Polynomial  = reflect(catalogue poly)      e.g. 0x8005 -> 0xA001
 *     Initial     = reflect(catalogue init)      e.g. 0xC6C6 -> 0x6363
 *     XorOut      = catalogue xorout             applied after reflection, unchanged
 *
 * All four built-in defaults have bit-symmetric initial values, which is
 * exactly why this is easy to miss: CRC-16/ISO-IEC-14443-3-A (init 0xC6C6)
 * yields its check value 0xBF05 only with Initial = 0x6363, and 0x1480 with
 * the catalogue value copied verbatim. test_crc.cpp locks both numbers.
 *
 * Not expressible here, by design: models with refin != refout, and widths
 * other than 8/16/32/64 (CRC-24, CRC-15/CAN, CRC-7 ...). A CRC-24 polynomial
 * handed to Crc32 compiles and computes something that is not CRC-24.
 */

/* CRC-8/SMBUS defaults: poly 0x07, init 0, xorout 0, non-reflected. */
template<
	class MethodT = Bitwise,
	uint8_t Polynomial = 0x07u,
	uint8_t Initial = 0x00u,
	uint8_t XorOut = 0x00u,
	bool Reflected = false,
	std::endian WireOrder = std::endian::big>
using Crc8 = detail::Engine<
	MethodT, uint8_t, Polynomial, Initial, XorOut, Reflected, WireOrder>;

/* CRC-16/MODBUS defaults: reflected poly 0xA001, init 0xFFFF, xorout 0. */
template<
	class MethodT = Bitwise,
	uint16_t Polynomial = 0xA001u,
	uint16_t Initial = 0xFFFFu,
	uint16_t XorOut = 0x0000u,
	bool Reflected = true,
	std::endian WireOrder = std::endian::little>
using Crc16 = detail::Engine<
	MethodT, uint16_t, Polynomial, Initial, XorOut, Reflected, WireOrder>;

/* CRC-32/ISO-HDLC defaults: reflected 0xEDB88320, init/xorout all ones. */
template<
	class MethodT = Bitwise,
	uint32_t Polynomial = 0xEDB88320u,
	uint32_t Initial = 0xFFFFFFFFu,
	uint32_t XorOut = 0xFFFFFFFFu,
	bool Reflected = true,
	std::endian WireOrder = std::endian::little>
using Crc32 = detail::Engine<
	MethodT, uint32_t, Polynomial, Initial, XorOut, Reflected, WireOrder>;

/* CRC-64/ECMA-182 defaults: poly 0x42F0E1EBA9EA3693, init/xorout 0. */
template<
	class MethodT = Bitwise,
	uint64_t Polynomial = UINT64_C(0x42F0E1EBA9EA3693),
	uint64_t Initial = UINT64_C(0x0000000000000000),
	uint64_t XorOut = UINT64_C(0x0000000000000000),
	bool Reflected = false,
	std::endian WireOrder = std::endian::big>
using Crc64 = detail::Engine<
	MethodT, uint64_t, Polynomial, Initial, XorOut, Reflected, WireOrder>;

using Crc8Bitwise = Crc8<Bitwise>;
using Crc8Table = Crc8<Table>;
using Crc16Bitwise = Crc16<Bitwise>;
using Crc16Table = Crc16<Table>;
using Crc32Bitwise = Crc32<Bitwise>;
using Crc32Table = Crc32<Table>;
using Crc64Bitwise = Crc64<Bitwise>;
using Crc64Table = Crc64<Table>;

/* No calculation and no wire bytes. The optimizer removes both RX/TX calls. */
class NoCrc final : public Codec<uint8_t, 0u, std::endian::little> {
public:
	[[nodiscard]] constexpr uint8_t calculate(
			std::span<const uint8_t>) const noexcept
	{
		return 0u;
	}
};

static_assert(Policy<Crc8Bitwise> && Policy<Crc8Table>);
static_assert(Policy<Crc16Bitwise> && Policy<Crc16Table>);
static_assert(Policy<Crc32Bitwise> && Policy<Crc32Table>);
static_assert(Policy<Crc64Bitwise> && Policy<Crc64Table>);
static_assert(Policy<NoCrc>);

/* Generic helpers use each policy's own result type, width, and codec. */
template<Policy PolicyT>
[[nodiscard]] constexpr typename PolicyT::value_type calculate(
		const std::span<const uint8_t> bytes,
		PolicyT& policy) noexcept
{
	return policy.calculate(bytes);
}

/*
 * The default-constructing conveniences are unconditionally noexcept, so a
 * policy whose default constructor may throw is refused here rather than
 * accepted and terminated at the first throw. Such a policy is still a valid
 * crc::Policy; construct it yourself and use the overloads taking a reference.
 */
template<Policy PolicyT>
	requires std::is_nothrow_default_constructible_v<PolicyT>
[[nodiscard]] constexpr typename PolicyT::value_type calculate(
		const std::span<const uint8_t> bytes) noexcept
{
	PolicyT policy{};
	return policy.calculate(bytes);
}

template<Policy PolicyT>
[[nodiscard]] constexpr bool verify(
		const std::span<const uint8_t> frame,
		PolicyT& policy) noexcept
{
	if (frame.size() < PolicyT::wire_size) {
		return false;
	}
	const std::size_t body_size = frame.size() - PolicyT::wire_size;
	const typename PolicyT::value_type expected =
		policy.calculate(frame.first(body_size));
	const uint8_t* trailer = frame.data();
	if constexpr (PolicyT::wire_size != 0u) {
		trailer += body_size;
	}
	const typename PolicyT::value_type received =
		policy.load(trailer);
	return expected == received;
}

template<Policy PolicyT>
	requires std::is_nothrow_default_constructible_v<PolicyT>
[[nodiscard]] constexpr bool verify(
		const std::span<const uint8_t> frame) noexcept
{
	PolicyT policy{};
	return crc::verify(frame, policy);
}

} // namespace crc

#endif /* CRC_CRC_H_ */
