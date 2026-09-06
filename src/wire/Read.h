/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Stateless, bounds-checked scalar and byte readers shared by protocol APIs.
 *
 * The caller owns the cursor. Every failed operation leaves both `offset` and
 * the output object unchanged, so parsing can be composed without hidden
 * mutable state. Endian selection is compile-time only; no reader contains a
 * runtime target-byte-order branch.
 */

#ifndef WIRE_READ_H_
#define WIRE_READ_H_

#include "Scalar.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace wire {

template<Scalar T>
	requires (!std::is_const_v<T>)
[[nodiscard]] inline bool read_native(
		const std::span<const uint8_t> data,
		std::size_t& offset,
		T& value) noexcept
{
	if (offset > data.size() || sizeof(T) > data.size() - offset) {
		return false;
	}
	const T result = detail::load_native<T>(data.data() + offset);
	value = result;
	offset += sizeof(T);
	return true;
}

template<EndianScalar T>
	requires (!std::is_const_v<T>)
[[nodiscard]] inline bool read_be(
		const std::span<const uint8_t> data,
		std::size_t& offset,
		T& value) noexcept
{
	if (offset > data.size() || sizeof(T) > data.size() - offset) {
		return false;
	}
	const T result = detail::load_ordered<std::endian::big, T>(
		data.data() + offset);
	value = result;
	offset += sizeof(T);
	return true;
}

template<EndianScalar T>
	requires (!std::is_const_v<T>)
[[nodiscard]] inline bool read_le(
		const std::span<const uint8_t> data,
		std::size_t& offset,
		T& value) noexcept
{
	if (offset > data.size() || sizeof(T) > data.size() - offset) {
		return false;
	}
	const T result = detail::load_ordered<std::endian::little, T>(
		data.data() + offset);
	value = result;
	offset += sizeof(T);
	return true;
}

[[nodiscard]] inline bool read_bytes(
		const std::span<const uint8_t> data,
		std::size_t& offset,
		const std::size_t count,
		std::span<const uint8_t>& value) noexcept
{
	if (offset > data.size() || count > data.size() - offset) {
		return false;
	}
	const std::span<const uint8_t> result = data.subspan(offset, count);
	offset += count;
	value = result;
	return true;
}

} // namespace wire

#endif /* WIRE_READ_H_ */
