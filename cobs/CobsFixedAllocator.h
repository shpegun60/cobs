/*
 * CobsFixedAllocator — the same policy contract (doc/COBS_ENGINE.md §9) over
 * static storage. No heap, no virtuals, no mutex.
 *
 * It implements EXACTLY the interface CobsHeapAllocator does, which is the
 * real test of the abstraction: if `Cobs` ever has to know which of the two it
 * is talking to, the abstraction has leaked.
 *
 * Two independent pools, because §9.1.3 requires RX and TX quotas to be
 * independent — a link that cannot transmit because the application is
 * holding received packets is a deadlock, not back-pressure.
 *
 * Placement note: the TX blocks live inside this object and the transport
 * reads them directly, so an instance backing a DMA transport belongs in
 * DMA-accessible RAM (§9.4). The RX pool has no such constraint — only the
 * CPU ever writes it.
 */

#ifndef COBS_FIXED_ALLOCATOR_H_
#define COBS_FIXED_ALLOCATOR_H_

#include "CobsEncoder.h"
#include "RxPacket.h"
#include "TxAllocation.h"
#include "detail/StaticBlockPool.h"

#include <cstddef>
#include <memory>
#include <new>

template<std::size_t RxMaxSize, std::size_t RxBlocks,
         std::size_t TxMaxSize, std::size_t TxBlocks>
class CobsFixedAllocator final {
public:
	static constexpr std::size_t rx_max_size = RxMaxSize;
	static constexpr std::size_t tx_max_size = TxMaxSize;

	// RxPacket only stores an Allocator*, so naming this incomplete type here
	// is legal — which is what lets the pool below be sized from sizeof(Packet).
	using Packet = RxPacket<CobsFixedAllocator>;

	static constexpr std::size_t rx_blocks = RxBlocks;
	static constexpr std::size_t tx_blocks = TxBlocks;

private:
	using RxPool = cobs_detail::StaticBlockPool<
		sizeof(Packet) + RxMaxSize, RxBlocks, alignof(Packet)>;
	// TX blocks are plain bytes: the payload needs no alignment beyond 1, and
	// the pool raises that on its own behalf for the free-list link.
	using TxPool = cobs_detail::StaticBlockPool<
		cobs_max_wire_size(TxMaxSize), TxBlocks, 1>;

public:
	// The defence against a policy declaring more than it can supply belongs
	// here, at compile time, inside the policy itself (§9.1.2).
	static_assert(RxPool::block_size >= sizeof(Packet) + rx_max_size,
		"the RX pool cannot hold a packet header plus rx_max_size bytes");
	static_assert(TxPool::block_size >= cobs_max_wire_size(tx_max_size),
		"the TX pool cannot hold the worst-case wire frame of tx_max_size bytes");

	using Stats = cobs_detail::PoolStats;

	CobsFixedAllocator() noexcept = default;
	CobsFixedAllocator(const CobsFixedAllocator&) = delete;
	CobsFixedAllocator& operator=(const CobsFixedAllocator&) = delete;

	[[nodiscard]] Packet* allocate_rx() noexcept
	{
		std::byte* const memory = m_rx.allocate();
		if (memory == nullptr) {
			return nullptr;
		}
		Packet* const packet = std::construct_at(
			reinterpret_cast<Packet*>(memory));
		packet->owner = this;
		return packet;
	}

	void deallocate_rx(Packet* const packet) noexcept
	{
		// The destructor runs through the pool's callback so that it can only
		// run AFTER the pointer has been validated: tearing down an object on
		// a foreign or already-freed block would be worse than the leak a
		// rejection costs (§9.1.2).
		(void)m_rx.release(reinterpret_cast<std::byte*>(packet),
		                   [](std::byte* const memory) noexcept {
			                   std::destroy_at(reinterpret_cast<Packet*>(memory));
		                   });
	}

	/*
	 * One slab: the request is honoured from the same block whatever its size,
	 * so this policy reports the FULL tx_max_size back as the capacity. The
	 * block was going to cost that much either way, and saying so means a
	 * message built on this policy essentially never grows — it can be created
	 * with no hint at all and filled to tx_max_size with one allocation, no
	 * reallocation and no copy.
	 *
	 * That is the whole point of reporting capacity rather than assuming it:
	 * on this policy the reserve is free, and only the policy knows that.
	 */
	[[nodiscard]] TxAllocation allocate_tx(const std::size_t requested) noexcept
	{
		if (requested > tx_max_size) {
			return {};
		}
		std::byte* const memory = m_tx.allocate();
		if (memory == nullptr) {
			return {};
		}
		return {memory, tx_max_size};
	}

	// A single size class has nothing to look up, so the capacity is ignored
	// here; a segregated policy would use it to pick the pool.
	void deallocate_tx(std::byte* const memory, std::size_t /*capacity*/) noexcept
	{
		m_tx.deallocate(memory);
	}

	/* Introspection below is NOT part of the policy contract (§9.1.4) — Cobs
	 * never asks any of this. It exists for applications sizing their pools
	 * and for tests. */
	[[nodiscard]] std::size_t rx_available() const noexcept { return m_rx.available(); }
	[[nodiscard]] std::size_t tx_available() const noexcept { return m_tx.available(); }
	[[nodiscard]] const Stats& rx_stats() const noexcept { return m_rx.stats(); }
	[[nodiscard]] const Stats& tx_stats() const noexcept { return m_tx.stats(); }

private:
	RxPool m_rx{};
	TxPool m_tx{};
};

#endif /* COBS_FIXED_ALLOCATOR_H_ */
