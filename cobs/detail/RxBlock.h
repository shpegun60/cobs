/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * cobs::RxBlock — the header the receiver constructs at the front of every RX
 * allocation, followed immediately by the payload in the same bytes.
 *
 * Storage never sees this type. The receiver asks its storage for
 * sizeof(RxBlock) + payload bytes, constructs the header in them and stamps
 * `owner`; the last Packet reference hands the same bytes back through that
 * owner. The block is trivially destructible on purpose, so there is nothing
 * to run before a release and a foreign pointer can be refused by a checking
 * pool before anything touches it.
 *
 * Application code only gets the immutable data() view; the receiver owns the
 * decoded size and queue linkage, while Packet owns reference arithmetic.
 */

#ifndef COBS_DETAIL_RX_BLOCK_H_
#define COBS_DETAIL_RX_BLOCK_H_

#include "../Format.h"
#include "../../wire/Storage.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace cobs {

template<class StorageT>
class Packet;

namespace detail {

template<class StorageT, class FormatT>
class Receiver;

/*
 * A never-defined stand-in for "some storage". RxBlock<AnyStorage> has the
 * layout of every RxBlock<S> — the only storage-dependent member is a pointer
 * — so the endpoint can size its block geometry from it BEFORE the real
 * storage type exists, and then assert the real block has the same shape.
 */
struct AnyStorage;

} // namespace detail

template<class StorageT>
struct RxBlock final {
	template<class, class>
	friend class detail::Receiver;
	friend class Packet<StorageT>;

	[[nodiscard]] std::span<const uint8_t> data() const noexcept
	{
		return {payload(), size};
	}

private:
	uint32_t refs = 1;
	uint16_t size = 0;
	RxBlock* next_ready = nullptr;
	StorageT* owner = nullptr;

	[[nodiscard]] std::span<uint8_t> writable_payload(
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
 * The physical block geometry an endpoint speaking `FormatT` binds its memory
 * to: RX blocks large enough for the header above plus the largest body, TX
 * blocks large enough for the worst-case encoded frame, both derived from the
 * Format and from the header's own size and alignment. A wire::BlockGeometry
 * keyed on the numbers alone, so every endpoint with the same Format binds the
 * same storage type — and so a test of the receiver on its own layer can bind
 * a storage exactly the way the endpoint does.
 *
 * The RX size is rounded to the header's alignment so an array of such
 * blocks keeps every slot aligned, not just the first; wire::Pool rounds the
 * same way, and the Geometry concept refuses anything that does not.
 */
template<class FormatT>
using GeometryFor = wire::BlockGeometry<
	wire::round_up(sizeof(RxBlock<AnyStorage>) + FormatT::max_receive_body,
	               alignof(RxBlock<AnyStorage>)),
	FormatT::tx_storage_size_for_capacity(FormatT::max_send_size),
	alignof(RxBlock<AnyStorage>)>;

} // namespace detail

} // namespace cobs

#endif /* COBS_DETAIL_RX_BLOCK_H_ */
