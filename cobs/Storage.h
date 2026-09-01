/*
 * Phase 0 storage contract.
 *
 * This concept deliberately describes the allocator-policy interface that the
 * current engine already uses. It adds one compile-time definition and useful
 * diagnostics without changing allocation, ownership, or wire behaviour.
 *
 * The final architecture described in doc/COBS_REFACTOR_PLAN.md will evolve
 * this vocabulary to Format/RxBlock/TxBlock and acquire/release. Keeping that
 * later migration separate is important: a checked current contract is a
 * guardrail for the rename, not an attempt to make the transitional names
 * permanent.
 */

#ifndef COBS_STORAGE_H_
#define COBS_STORAGE_H_

#include "TxAllocation.h"

#include <concepts>
#include <cstddef>

namespace cobs {

template<class T>
concept Storage = requires(
	T& storage,
	const std::size_t size,
	typename T::Packet* packet,
	std::byte* memory)
{
	typename T::Packet;

	{ T::rx_max_size } -> std::convertible_to<std::size_t>;
	{ T::tx_max_size } -> std::convertible_to<std::size_t>;

	{ storage.allocate_rx(size) }
		noexcept -> std::same_as<typename T::Packet*>;

	{ storage.deallocate_rx(packet) }
		noexcept -> std::same_as<void>;

	{ storage.allocate_tx(size) }
		noexcept -> std::same_as<TxAllocation>;

	{ storage.deallocate_tx(memory, size) }
		noexcept -> std::same_as<void>;
};

} // namespace cobs

#endif /* COBS_STORAGE_H_ */
