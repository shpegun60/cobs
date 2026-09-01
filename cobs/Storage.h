/*
 * Checked storage contract during the staged vocabulary migration.
 *
 * This concept deliberately describes the allocator-policy interface that the
 * current engine already uses. It adds one compile-time definition and useful
 * diagnostics without changing allocation, ownership, or wire behaviour.
 *
 * Format is already the sole source of protocol geometry. The remaining
 * migration described in doc/COBS_REFACTOR_PLAN.md replaces Packet and
 * TxAllocation vocabulary and then moves allocate/deallocate to
 * acquire/release. Keeping those ownership changes separate makes this
 * checked current contract a guardrail rather than a compatibility facade.
 */

#ifndef COBS_STORAGE_H_
#define COBS_STORAGE_H_

#include "Format.h"
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
	typename T::Format;

	{ T::Format::max_receive_size } -> std::convertible_to<std::size_t>;
	{ T::Format::max_send_size } -> std::convertible_to<std::size_t>;

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
