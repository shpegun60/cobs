/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * modbus::rtu::RxBlock — the header the receiver constructs at the front of
 * every RX allocation, followed by the complete validated ADU in the same
 * bytes. Storage never sees this type; the receiver asks for
 * sizeof(RxBlock) + adu bytes and constructs the header itself.
 *
 * Address and function live in what would otherwise be padding after the
 * 16-bit size, so the header is 16 bytes on a 32-bit target, the same as the
 * COBS one. Trivially destructible on purpose: nothing runs before a release.
 */

#ifndef MODBUS_RTU_DETAIL_RX_BLOCK_H_
#define MODBUS_RTU_DETAIL_RX_BLOCK_H_

#include "../../../wire/Storage.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace modbus::rtu {

template<class StorageT, class LayoutT>
class Packet;

namespace detail {

template<class StorageT, class LayoutT>
class Receiver;

// A never-defined stand-in for "some storage": RxBlock<AnyStorage> has the
// layout of every RxBlock<S>, so the endpoint can size its geometry before
// the real storage type exists (see cobs/detail/RxBlock.h for the reasoning).
struct AnyStorage;

} // namespace detail

template<class StorageT>
struct RxBlock final {
	template<class, class>
	friend class Packet;
	template<class, class>
	friend class detail::Receiver;

private:
	uint32_t refs = 1;
	uint16_t adu_size = 0;
	uint8_t address = 0;
	uint8_t function = 0;
	RxBlock* next_ready = nullptr;
	StorageT* owner = nullptr;

	[[nodiscard]] std::span<uint8_t> writable_adu(
			const std::size_t allocated) noexcept
	{
		return {payload(), allocated};
	}

	[[nodiscard]] uint8_t* payload() noexcept
	{
		return reinterpret_cast<uint8_t*>(this) + sizeof(RxBlock);
	}

	[[nodiscard]] const uint8_t* payload() const noexcept
	{
		return reinterpret_cast<const uint8_t*>(this) + sizeof(RxBlock);
	}
};

namespace detail {

/*
 * The block geometry an endpoint with `LayoutT` binds its memory to: RX
 * blocks for the header plus one complete ADU, TX blocks for one complete
 * ADU, keyed on the numbers alone so that every Format sharing a Layout —
 * Crc16Bitwise and Crc16Table, say — binds the same storage type. See
 * cobs/detail/RxBlock.h for the rounding rule.
 */
template<class LayoutT>
using GeometryFor = wire::BlockGeometry<
	wire::round_up(sizeof(RxBlock<AnyStorage>) + LayoutT::max_adu_size,
	               alignof(RxBlock<AnyStorage>)),
	LayoutT::max_adu_size,
	alignof(RxBlock<AnyStorage>)>;

} // namespace detail

} // namespace modbus::rtu

#endif /* MODBUS_RTU_DETAIL_RX_BLOCK_H_ */
