/*
 * FixedPoolAllocator — BlockCount packets of PayloadCapacity bytes each.
 *
 * A thin RX adapter over detail::StaticBlockPool: the pool owns the memory
 * mechanics (free list, validation, statistics), this owns the interpretation
 * — each block is one RxPacket header followed by its payload. The same pool
 * primitive backs TX blocks, which are plain bytes; that is the split, and it
 * is why a user supplying memory writes one backend rather than two.
 *
 * Contract: doc/COBS_ENGINE.md §4.1 and §6. The allocator does NOT define the
 * protocol limit; it advertises what it can hold and the protocol asserts that
 * this is enough:
 *
 *     static_assert(Allocator::payload_capacity >= MaxDecodedSize);
 *
 * The template parameter is the PAYLOAD capacity, not the block size, and
 * that direction matters. A block is a header plus a payload, and the
 * header's size is an ABI property: 24 bytes on x86-64, 12 on Cortex-M. Were
 * the block size the knob, the same configuration would accept different wire
 * frames on different platforms — the platform deciding protocol semantics.
 * Parameterized this way,
 *
 *     FixedPoolAllocator<1024, 8>
 *
 * means "eight packets of exactly 1024 decoded bytes" everywhere, and the ABI
 * only moves the RAM cost (1048 bytes per block on x86-64, 1036 on Cortex-M).
 */

#ifndef COBS_FIXED_POOL_ALLOCATOR_H_
#define COBS_FIXED_POOL_ALLOCATOR_H_

#include "RxPacket.h"
#include "detail/StaticBlockPool.h"

#include <cstddef>
#include <memory>
#include <new>

template<std::size_t PayloadCapacity, std::size_t BlockCount>
class FixedPoolAllocator final {
public:
	// RxPacket only stores an Allocator*, so naming this incomplete type here
	// is legal. It is used for the pool's geometry, never to derive a number
	// the protocol depends on.
	using Packet = RxPacket<FixedPoolAllocator>;

	static constexpr std::size_t payload_capacity = PayloadCapacity;
	static constexpr std::size_t block_count = BlockCount;
	// Logical content of one block. The pool may spend more per block (its
	// own alignment and free-list link); see detail::StaticBlockPool.
	static constexpr std::size_t storage_size = sizeof(Packet) + PayloadCapacity;

private:
	// Alignment is requested for the Packet placed at the block's start. The
	// pool raises it further if its own bookkeeping needs more, which is why
	// this adapter no longer has to assert that a Packet happens to be
	// pointer-aligned: that is now the pool's problem, structurally.
	using Pool = cobs::detail::StaticBlockPool<storage_size, BlockCount, alignof(Packet)>;

public:
	using Stats = typename Pool::Stats;

	FixedPoolAllocator() noexcept = default;
	FixedPoolAllocator(const FixedPoolAllocator&) = delete;
	FixedPoolAllocator& operator=(const FixedPoolAllocator&) = delete;

	// A constructed packet owning the rest of its block, or nullptr when the
	// pool is dry. Never blocks, never allocates.
	[[nodiscard]] Packet* allocate() noexcept
	{
		std::byte* const memory = m_pool.allocate();
		if (memory == nullptr) {
			return nullptr;
		}
		Packet* const p = ::new (static_cast<void*>(memory)) Packet{};
		p->owner = this;
		return p;
	}

	void deallocate(Packet* const p) noexcept
	{
		// The destructor runs through the pool's callback rather than here,
		// so it can only run AFTER the pointer has been validated: tearing
		// down an object on a foreign or already-freed block would be worse
		// than the leak the rejection costs.
		(void)m_pool.release(reinterpret_cast<std::byte*>(p),
		                     [](std::byte* const memory) noexcept {
			                     std::destroy_at(reinterpret_cast<Packet*>(memory));
		                     });
	}

	[[nodiscard]] std::size_t available() const noexcept { return m_pool.available(); }
	[[nodiscard]] const Stats& stats() const noexcept { return m_pool.stats(); }

private:
	Pool m_pool{};
};

#endif /* COBS_FIXED_POOL_ALLOCATOR_H_ */
