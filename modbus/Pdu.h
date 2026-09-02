/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Stateless, bounds-checked native/BE/LE readers for Modbus function data.
 *
 * Packet owns no cursor. The application owns `offset`, so two independent
 * parsers can inspect one immutable Packet without mutating shared state.
 * Every failure leaves both offset and output unchanged. Endian selection is
 * compile-time; these helpers never inspect target byte order at runtime.
 */

#ifndef MODBUS_PDU_H_
#define MODBUS_PDU_H_

#include "Types.h"
#include "../wire/Scalar.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace modbus {

template<wire::Scalar T>
	requires (!std::is_const_v<T>)
[[nodiscard]] inline bool read_native(
		const std::span<const uint8_t> data,
		std::size_t& offset,
		T& value) noexcept
{
	if (offset > data.size() || sizeof(T) > data.size() - offset) {
		return false;
	}
	const T result = wire::detail::load_native<T>(data.data() + offset);
	value = result;
	offset += sizeof(T);
	return true;
}

template<wire::EndianScalar T>
	requires (!std::is_const_v<T>)
[[nodiscard]] inline bool read_be(
		const std::span<const uint8_t> data,
		std::size_t& offset,
		T& value) noexcept
{
	if (offset > data.size() || sizeof(T) > data.size() - offset) {
		return false;
	}
	const T result = wire::detail::load_ordered<std::endian::big, T>(
		data.data() + offset);
	value = result;
	offset += sizeof(T);
	return true;
}

template<wire::EndianScalar T>
	requires (!std::is_const_v<T>)
[[nodiscard]] inline bool read_le(
		const std::span<const uint8_t> data,
		std::size_t& offset,
		T& value) noexcept
{
	if (offset > data.size() || sizeof(T) > data.size() - offset) {
		return false;
	}
	const T result = wire::detail::load_ordered<std::endian::little, T>(
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

} // namespace modbus

#endif /* MODBUS_PDU_H_ */
