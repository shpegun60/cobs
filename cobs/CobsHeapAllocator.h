/*
 * CobsHeapAllocator — the reference implementation of the allocator policy
 * (doc/COBS_ENGINE.md §9), and the default for `Cobs<>`.
 *
 * It is the smallest thing that can satisfy the contract, which makes it the
 * example to read before writing your own: two constants, four functions, and
 * no state at all.
 *
 * The limits are template parameters rather than "unbounded" because
 * unbounded is not available to us — the zero-copy RX decoder is handed its
 * final output span at NeedOutput, before a single payload byte has arrived,
 * so a number has to be committed to up front (§9.2).
 */

#ifndef COBS_HEAP_ALLOCATOR_H_
#define COBS_HEAP_ALLOCATOR_H_

#include "CobsEncoder.h"
#include "RxPacket.h"
#include "TxAllocation.h"

#include <cstddef>
#include <memory>
#include <new>

template<std::size_t RxMaxSize = 1024, std::size_t TxMaxSize = 1024>
class CobsHeapAllocator final {
public:
	static constexpr std::size_t rx_max_size = RxMaxSize;
	static constexpr std::size_t tx_max_size = TxMaxSize;

	using Packet = RxPacket<CobsHeapAllocator>;

	CobsHeapAllocator() noexcept = default;
	CobsHeapAllocator(const CobsHeapAllocator&) = delete;
	CobsHeapAllocator& operator=(const CobsHeapAllocator&) = delete;

	// One contiguous [RxPacket][payload] region, as §9.1.2 requires. A heap
	// policy has no trouble honouring that, so heap and pool end up with
	// identical geometry and differ only in where the region came from.
	[[nodiscard]] Packet* allocate_rx() noexcept
	{
		void* const memory =
			::operator new(sizeof(Packet) + rx_max_size, std::nothrow);
		if (memory == nullptr) {
			return nullptr;
		}
		Packet* const packet = std::construct_at(static_cast<Packet*>(memory));
		packet->owner = this;
		return packet;
	}

	void deallocate_rx(Packet* const packet) noexcept
	{
		if (packet == nullptr) {
			return;
		}
		std::destroy_at(packet);
		::operator delete(static_cast<void*>(packet));
	}

	/*
	 * Exactly the capacity asked for, and a block sized for exactly that.
	 * This is where the sized TX contract pays: a seven-byte message costs
	 * nine bytes here, not cobs_max_wire_size of the largest frame the policy
	 * allows — for tx_max_size = 1024 that is 9 against 1030.
	 *
	 * No rounding up, no size classes, no growth rule. Deciding how much to
	 * ask for is the CONTAINER's job (§9.1.0): CobsMsg knows its current
	 * capacity and what it needs, so it computes the geometric target and asks
	 * once. A policy that also had an opinion would be two growth rules
	 * fighting over one allocation.
	 *
	 * A request for zero still gets real storage — cobs_max_wire_size(0) is
	 * two bytes, enough for the canonical empty frame `01 00` — while the
	 * reported payload capacity is honestly zero.
	 */
	[[nodiscard]] TxAllocation allocate_tx(const std::size_t requested) noexcept
	{
		if (requested > tx_max_size) {
			return {};
		}
		void* const memory =
			::operator new(cobs_max_wire_size(requested), std::nothrow);
		if (memory == nullptr) {
			return {};
		}
		return {static_cast<std::byte*>(memory), requested};
	}

	// The capacity comes back for the benefit of policies that segregate by
	// size class; this one deliberately ignores it. Sized operator delete
	// would be an ABI- and runtime-dependent optimisation, and measuring it
	// belongs in a benchmark, not in a contract argument.
	void deallocate_tx(std::byte* const memory, std::size_t /*capacity*/) noexcept
	{
		::operator delete(static_cast<void*>(memory));
	}
};

#endif /* COBS_HEAP_ALLOCATOR_H_ */
