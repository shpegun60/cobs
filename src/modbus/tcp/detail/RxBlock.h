/* Author: shpegun60; SPDX-License-Identifier: MIT */
#ifndef MODBUS_TCP_DETAIL_RX_BLOCK_H_
#define MODBUS_TCP_DETAIL_RX_BLOCK_H_
#include "../../../wire/Storage.h"
#include <cstddef>
#include <cstdint>

namespace modbus::tcp {
template<class, class> class Packet;
namespace detail {
template<class, class> class Receiver;
struct AnyStorage;

template<class StorageT>
class RxBlock final {
	template<class, class> friend class modbus::tcp::Packet;
	template<class, class> friend class Receiver;
	uint32_t refs = 1u;
	uint32_t adu_size = 0u; // MBAP permits 65541 physical bytes, not just 65535
	RxBlock* next_ready = nullptr;
	StorageT* owner = nullptr;
	[[nodiscard]] uint8_t* payload() noexcept
	{
		return reinterpret_cast<uint8_t*>(this) + sizeof(RxBlock);
	}
	[[nodiscard]] const uint8_t* payload() const noexcept
	{
		return reinterpret_cast<const uint8_t*>(this) + sizeof(RxBlock);
	}
};

template<class LayoutT>
using GeometryFor = wire::BlockGeometry<
	wire::round_up(sizeof(RxBlock<AnyStorage>) + LayoutT::max_adu_size,
		alignof(RxBlock<AnyStorage>)),
	LayoutT::max_adu_size, alignof(RxBlock<AnyStorage>)>;
} // namespace detail
} // namespace modbus::tcp
#endif
