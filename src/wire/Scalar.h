/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Shared scalar wire primitives for the COBS and Modbus builders/readers.
 *
 * Native operations copy the target's object representation unchanged.
 * Ordered operations produce the requested byte order at compile time.  They
 * contain no runtime endianness branch: std::endian::native and Order are both
 * constants, so only the matching implementation is instantiated.
 */

#ifndef WIRE_SCALAR_H_
#define WIRE_SCALAR_H_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#if defined(_MSC_VER)
#define WIRE_SCALAR_FORCE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define WIRE_SCALAR_FORCE_INLINE inline __attribute__((always_inline))
#else
#define WIRE_SCALAR_FORCE_INLINE inline
#endif

namespace wire {

namespace detail {

template<class T, bool = std::is_enum_v<T>>
struct IsBoolBackedEnum : std::false_type {};

template<class T>
struct IsBoolBackedEnum<T, true>
	: std::bool_constant<std::is_same_v<std::underlying_type_t<T>, bool>> {};

template<class T>
inline constexpr bool is_bool_backed_enum_v = IsBoolBackedEnum<T>::value;

} // namespace detail

/*
 * A scalar whose native object representation is meaningful enough to append
 * deliberately.  Padded structs, pointers, bool, bool-backed enums and
 * volatile MMIO objects are rejected at the call boundary.
 */
template<class T>
concept Scalar =
	!std::is_volatile_v<T> &&
	!detail::is_bool_backed_enum_v<std::remove_cv_t<T>> &&
	((std::is_arithmetic_v<std::remove_cv_t<T>> &&
	  !std::is_same_v<std::remove_cv_t<T>, bool>) ||
	 std::is_enum_v<std::remove_cv_t<T>> ||
	 std::is_same_v<std::remove_cv_t<T>, std::byte>);

/*
 * Explicit endian conversion is intentionally narrower than a native copy.
 * It supports the scalar widths with a conventional byte-order meaning and
 * rejects long double and any unusual-width arithmetic type.  float/double
 * order their existing object representation; peers must still agree on the
 * floating-point representation itself.
 */
template<class T>
concept EndianScalar =
	Scalar<T> &&
	(sizeof(std::remove_cv_t<T>) == 1u ||
	 sizeof(std::remove_cv_t<T>) == 2u ||
	 sizeof(std::remove_cv_t<T>) == 4u ||
	 sizeof(std::remove_cv_t<T>) == 8u) &&
	(std::is_integral_v<std::remove_cv_t<T>> ||
	 std::is_enum_v<std::remove_cv_t<T>> ||
	 std::is_same_v<std::remove_cv_t<T>, std::byte> ||
	 std::is_same_v<std::remove_cv_t<T>, float> ||
	 std::is_same_v<std::remove_cv_t<T>, double>);

namespace detail {

template<class T>
using Unqualified = std::remove_cv_t<T>;

template<class T>
using Bits = std::conditional_t<
	(sizeof(Unqualified<T>) == 1u), uint8_t,
	std::conditional_t<
		(sizeof(Unqualified<T>) == 2u), uint16_t,
		std::conditional_t<(sizeof(Unqualified<T>) == 4u), uint32_t, uint64_t>>>;

[[nodiscard]] WIRE_SCALAR_FORCE_INLINE constexpr uint16_t byte_swap(
		const uint16_t value) noexcept
{
	return static_cast<uint16_t>(
		static_cast<uint16_t>(value << 8u) |
		static_cast<uint16_t>(value >> 8u));
}

[[nodiscard]] WIRE_SCALAR_FORCE_INLINE constexpr uint32_t byte_swap(
		const uint32_t value) noexcept
{
	return ((value & 0x000000FFu) << 24u) |
	       ((value & 0x0000FF00u) << 8u) |
	       ((value & 0x00FF0000u) >> 8u) |
	       ((value & 0xFF000000u) >> 24u);
}

[[nodiscard]] WIRE_SCALAR_FORCE_INLINE constexpr uint64_t byte_swap(
		const uint64_t value) noexcept
{
	return ((value & UINT64_C(0x00000000000000FF)) << 56u) |
	       ((value & UINT64_C(0x000000000000FF00)) << 40u) |
	       ((value & UINT64_C(0x0000000000FF0000)) << 24u) |
	       ((value & UINT64_C(0x00000000FF000000)) << 8u) |
	       ((value & UINT64_C(0x000000FF00000000)) >> 8u) |
	       ((value & UINT64_C(0x0000FF0000000000)) >> 24u) |
	       ((value & UINT64_C(0x00FF000000000000)) >> 40u) |
	       ((value & UINT64_C(0xFF00000000000000)) >> 56u);
}

static_assert(byte_swap(uint16_t{0x1234u}) == uint16_t{0x3412u});
static_assert(byte_swap(uint32_t{0x01020304u}) == uint32_t{0x04030201u});
static_assert(byte_swap(UINT64_C(0x0102030405060708)) ==
	          UINT64_C(0x0807060504030201));

/*
 * ARM targets that cannot promise unaligned scalar access use bytewise copies
 * in the abstract machine. That is both alignment-safe and a useful
 * code-generation contract:
 *
 * - ARMv7-M/ARMv8-M with unaligned access enabled folds these operations into
 *   one LDR/STR, plus REV/REV16 when the requested order differs;
 * - ARMv6-M and builds using -mno-unaligned-access keep inline LDRB/STRB
 *   sequences instead of calling the out-of-line memcpy helper for 2/4/8
 *   bytes.
 *
 * Other targets keep fixed-size memcpy, allowing ARMv7-M/ARMv8-M and x86 to
 * use their efficient unaligned scalar instructions. The decision below is
 * an ACLE/preprocessor constant; it never exists in runtime code. The loop
 * bound and Order are template constants too, so optimized bytewise builds
 * fully unroll the loop.
 */
#if (defined(__arm__) || defined(__thumb__)) && \
    !defined(__ARM_FEATURE_UNALIGNED)
inline constexpr bool requires_bytewise_scalar_io = true;
#else
inline constexpr bool requires_bytewise_scalar_io = false;
#endif

template<std::endian Order, class U>
	requires (std::is_unsigned_v<U>)
WIRE_SCALAR_FORCE_INLINE void store_bits(
		uint8_t* const destination,
		const U value) noexcept
{
	static_assert(Order == std::endian::little || Order == std::endian::big);
	static_assert(
		sizeof(U) == 1u || sizeof(U) == 2u ||
		sizeof(U) == 4u || sizeof(U) == 8u);

	if constexpr (sizeof(U) == 1u) {
		destination[0] = static_cast<uint8_t>(value);
	} else if constexpr (sizeof(U) == 2u) {
		if constexpr (Order == std::endian::little) {
			destination[0] = static_cast<uint8_t>(value);
			destination[1] = static_cast<uint8_t>(value >> 8u);
		} else {
			destination[0] = static_cast<uint8_t>(value >> 8u);
			destination[1] = static_cast<uint8_t>(value);
		}
	} else if constexpr (sizeof(U) == 4u) {
		if constexpr (Order == std::endian::little) {
			destination[0] = static_cast<uint8_t>(value);
			destination[1] = static_cast<uint8_t>(value >> 8u);
			destination[2] = static_cast<uint8_t>(value >> 16u);
			destination[3] = static_cast<uint8_t>(value >> 24u);
		} else {
			destination[0] = static_cast<uint8_t>(value >> 24u);
			destination[1] = static_cast<uint8_t>(value >> 16u);
			destination[2] = static_cast<uint8_t>(value >> 8u);
			destination[3] = static_cast<uint8_t>(value);
		}
	} else {
		/* Split at 32 bits so ARMv6-M never needs an __aeabi 64-bit shift. */
		const uint32_t low = static_cast<uint32_t>(value);
		const uint32_t high = static_cast<uint32_t>(value >> 32u);
		if constexpr (Order == std::endian::little) {
			store_bits<std::endian::little>(destination, low);
			store_bits<std::endian::little>(destination + 4u, high);
		} else {
			store_bits<std::endian::big>(destination, high);
			store_bits<std::endian::big>(destination + 4u, low);
		}
	}
}

template<std::endian Order, class U>
	requires (std::is_unsigned_v<U>)
[[nodiscard]] WIRE_SCALAR_FORCE_INLINE U load_bits(
		const uint8_t* const source) noexcept
{
	static_assert(Order == std::endian::little || Order == std::endian::big);
	static_assert(
		sizeof(U) == 1u || sizeof(U) == 2u ||
		sizeof(U) == 4u || sizeof(U) == 8u);

	if constexpr (sizeof(U) == 1u) {
		return static_cast<U>(source[0]);
	} else if constexpr (sizeof(U) == 2u) {
		if constexpr (Order == std::endian::little) {
			return static_cast<U>(
				static_cast<U>(source[0]) |
				static_cast<U>(static_cast<U>(source[1]) << 8u));
		} else {
			return static_cast<U>(
				static_cast<U>(static_cast<U>(source[0]) << 8u) |
				static_cast<U>(source[1]));
		}
	} else if constexpr (sizeof(U) == 4u) {
		if constexpr (Order == std::endian::little) {
			return static_cast<U>(source[0]) |
			       (static_cast<U>(source[1]) << 8u) |
			       (static_cast<U>(source[2]) << 16u) |
			       (static_cast<U>(source[3]) << 24u);
		} else {
			return (static_cast<U>(source[0]) << 24u) |
			       (static_cast<U>(source[1]) << 16u) |
			       (static_cast<U>(source[2]) << 8u) |
			       static_cast<U>(source[3]);
		}
	} else {
		uint32_t low = 0u;
		uint32_t high = 0u;
		if constexpr (Order == std::endian::little) {
			low = load_bits<std::endian::little, uint32_t>(source);
			high = load_bits<std::endian::little, uint32_t>(source + 4u);
		} else {
			high = load_bits<std::endian::big, uint32_t>(source);
			low = load_bits<std::endian::big, uint32_t>(source + 4u);
		}
		return static_cast<U>(
			static_cast<uint64_t>(low) |
			(static_cast<uint64_t>(high) << 32u));
	}
}

template<Scalar T>
WIRE_SCALAR_FORCE_INLINE void store_native(
		uint8_t* const destination,
		const T& value) noexcept
{
	static_assert(
		std::endian::native == std::endian::little ||
		std::endian::native == std::endian::big,
		"wire native scalar copy requires a purely little- or big-endian target");

	if constexpr (requires_bytewise_scalar_io &&
	              (sizeof(T) == 1u || sizeof(T) == 2u ||
	               sizeof(T) == 4u || sizeof(T) == 8u)) {
		const Bits<T> bits = std::bit_cast<Bits<T>>(value);
		store_bits<std::endian::native>(destination, bits);
	} else {
		std::memcpy(destination, &value, sizeof(T));
	}
}

template<std::endian Order, EndianScalar T>
WIRE_SCALAR_FORCE_INLINE void store_ordered(
		uint8_t* const destination,
		const T& value) noexcept
{
	static_assert(Order == std::endian::little || Order == std::endian::big);
	static_assert(
		std::endian::native == std::endian::little ||
		std::endian::native == std::endian::big,
		"wire endian conversion requires a purely little- or big-endian target");

	if constexpr (sizeof(T) == 1u || std::endian::native == Order) {
		store_native(destination, value);
	} else {
		const Bits<T> native = std::bit_cast<Bits<T>>(value);
		const Bits<T> ordered = byte_swap(native);
		store_native(destination, ordered);
	}
}

template<std::endian Order, EndianScalar T>
inline void store_ordered(
		uint8_t* const destination,
		const std::span<const T> values) noexcept
{
	static_assert(Order == std::endian::little || Order == std::endian::big);
	static_assert(
		std::endian::native == std::endian::little ||
		std::endian::native == std::endian::big,
		"wire endian conversion requires a purely little- or big-endian target");

	if (values.empty()) {
		return;
	}
	if constexpr (sizeof(T) == 1u || std::endian::native == Order) {
		std::memcpy(destination, values.data(), values.size_bytes());
	} else {
		uint8_t* output = destination;
		for (const T& value : values) {
			store_ordered<Order>(output, value);
			output += sizeof(T);
		}
	}
}

template<Scalar T>
[[nodiscard]] WIRE_SCALAR_FORCE_INLINE Unqualified<T> load_native(
		const uint8_t* const source) noexcept
{
	static_assert(
		std::endian::native == std::endian::little ||
		std::endian::native == std::endian::big,
		"wire native scalar copy requires a purely little- or big-endian target");

	if constexpr (requires_bytewise_scalar_io &&
	              (sizeof(T) == 1u || sizeof(T) == 2u ||
	               sizeof(T) == 4u || sizeof(T) == 8u)) {
		const Bits<T> bits = load_bits<std::endian::native, Bits<T>>(source);
		return std::bit_cast<Unqualified<T>>(bits);
	} else {
		Unqualified<T> value{};
		std::memcpy(&value, source, sizeof(value));
		return value;
	}
}

template<std::endian Order, EndianScalar T>
[[nodiscard]] WIRE_SCALAR_FORCE_INLINE Unqualified<T> load_ordered(
		const uint8_t* const source) noexcept
{
	static_assert(Order == std::endian::little || Order == std::endian::big);
	static_assert(
		std::endian::native == std::endian::little ||
		std::endian::native == std::endian::big,
		"wire endian conversion requires a purely little- or big-endian target");

	if constexpr (sizeof(T) == 1u || std::endian::native == Order) {
		return load_native<T>(source);
	} else {
		const Bits<T> native = load_native<Bits<T>>(source);
		const Bits<T> ordered = byte_swap(native);
		return std::bit_cast<Unqualified<T>>(ordered);
	}
}

} // namespace detail
} // namespace wire

#undef WIRE_SCALAR_FORCE_INLINE

#endif /* WIRE_SCALAR_H_ */
