/*
 * FixedPoolAllocator — BlockCount packets of PayloadCapacity bytes each, in
 * static storage. No heap, no virtuals, no mutex, O(1) allocate and
 * deallocate.
 *
 * Contract: doc/COBS_ENGINE.md §4.1 and §6. The allocator does NOT define the
 * protocol limit; it advertises what it can hold and the protocol asserts that
 * this is enough:
 *
 *     static_assert(Allocator::payload_capacity >= MaxDecodedSize);
 *
 * The template parameter is the PAYLOAD capacity, not the block size, and
 * that direction matters. A block is one RxPacket header followed by its
 * payload, and the header's size is an ABI property: 24 bytes on x86-64, 12 on
 * Cortex-M. Were the block size the knob, the same configuration would accept
 * different wire frames on different platforms — the platform would be
 * deciding protocol semantics. Parameterized this way,
 *
 *     FixedPoolAllocator<1024, 8>
 *
 * means "eight packets of exactly 1024 decoded bytes" everywhere, and the ABI
 * only moves the RAM cost (1048 bytes per block on x86-64, 1036 on Cortex-M).
 *
 * storage_size is the logical block content. It is deliberately NOT promised
 * to equal sizeof(Block): alignment may add tail padding, and a name that
 * implied otherwise would eventually start an investigation into the
 * compiler's alleged crimes.
 *
 * Free blocks are threaded on an intrusive list stored in the blocks
 * themselves, so the pool costs no bookkeeping memory beyond one pointer.
 *
 * Validation (double free, foreign or misaligned pointer) is compiled in only
 * when COBS_POOL_CHECKS is on — by default in debug builds and in the test
 * suite, never on the hot path of a release build. A rejected deallocate is
 * counted and ignored: leaking one block is a far better failure than
 * corrupting the free list.
 */

#ifndef COBS_FIXED_POOL_ALLOCATOR_H_
#define COBS_FIXED_POOL_ALLOCATOR_H_

#include "RxPacket.h"

#include <cstddef>
#include <cstdint>
#include <new>

#ifndef COBS_POOL_CHECKS
#	ifdef NDEBUG
#		define COBS_POOL_CHECKS 0
#	else
#		define COBS_POOL_CHECKS 1
#	endif
#endif

template<std::size_t PayloadCapacity, std::size_t BlockCount>
class FixedPoolAllocator final {
public:
	// RxPacket only stores an Allocator*, so naming this incomplete type here
	// is legal. It is now used only for the pool's internal geometry, never
	// to derive a number the protocol depends on.
	using Packet = RxPacket<FixedPoolAllocator>;

	static constexpr std::size_t payload_capacity = PayloadCapacity;
	static constexpr std::size_t block_count = BlockCount;
	// Logical content of one block; sizeof(Block) may exceed this by padding.
	static constexpr std::size_t storage_size = sizeof(Packet) + PayloadCapacity;

	static_assert(BlockCount >= 1, "a pool needs at least one block");
	static_assert(storage_size >= sizeof(void*),
		"a free block must be able to hold the free-list link");

	struct Stats {
		uint32_t in_use   = 0; // blocks currently handed out
		uint32_t high_water = 0;
		uint32_t exhausted  = 0; // allocate() calls that found the pool dry
		uint32_t rejected   = 0; // deallocate() calls refused by the checks
	};

	FixedPoolAllocator() noexcept
	{
		// Thread every block onto the free list, first block at the head.
		for (std::size_t i = BlockCount; i > 0; --i) {
			Block* const b = &m_blocks[i - 1];
			link_of(b) = m_free;
			m_free = b;
		}
	}

	FixedPoolAllocator(const FixedPoolAllocator&) = delete;
	FixedPoolAllocator& operator=(const FixedPoolAllocator&) = delete;

	// Hands out a constructed packet owning the rest of its block, or nullptr
	// when the pool is dry. Never blocks, never allocates.
	[[nodiscard]] Packet* allocate() noexcept
	{
		Block* const b = m_free;
		if (b == nullptr) {
			++m_stats.exhausted;
			return nullptr;
		}
		m_free = link_of(b);

		++m_stats.in_use;
		if (m_stats.in_use > m_stats.high_water) {
			m_stats.high_water = m_stats.in_use;
		}

		Packet* const p = ::new (static_cast<void*>(b->bytes)) Packet{};
		p->owner = this;
		return p;
	}

	void deallocate(Packet* const p) noexcept
	{
		if (p == nullptr) {
			return;
		}
		Block* const b = block_of(p);
#if COBS_POOL_CHECKS
		if (!owns(b) || is_free(b)) {
			++m_stats.rejected; // foreign, misaligned or already free
			return;
		}
#endif
		p->~Packet();
		link_of(b) = m_free;
		m_free = b;
		--m_stats.in_use;
	}

	[[nodiscard]] std::size_t available() const noexcept
	{
		return BlockCount - m_stats.in_use;
	}
	[[nodiscard]] const Stats& stats() const noexcept { return m_stats; }

private:
	// Aligned for what actually lives here — a Packet — not for the strictest
	// type in the language. Nothing DMAs into this pool (the transport fills
	// its own chunks; COBS decodes CPU-side), so max_align_t would only waste
	// up to 15 bytes per block. A Packet contains pointers, so its alignment
	// also covers the free-list link stored in a free block.
	struct alignas(Packet) Block {
		unsigned char bytes[storage_size];
	};

	// A free block stores the link to the next free block in its own storage;
	// a block is never both linked and in use, so this cannot alias live data.
	static Block*& link_of(Block* const b) noexcept
	{
		return *reinterpret_cast<Block**>(b->bytes);
	}

	Block* block_of(Packet* const p) noexcept
	{
		return reinterpret_cast<Block*>(reinterpret_cast<unsigned char*>(p));
	}

#if COBS_POOL_CHECKS
	// Inside this pool, and exactly on a block boundary — a pointer into the
	// middle of a block is as wrong as a pointer from somewhere else.
	bool owns(const Block* const b) const noexcept
	{
		if (b < &m_blocks[0] || b > &m_blocks[BlockCount - 1]) {
			return false;
		}
		const auto off = reinterpret_cast<const unsigned char*>(b) -
		                 reinterpret_cast<const unsigned char*>(&m_blocks[0]);
		return (static_cast<std::size_t>(off) % sizeof(Block)) == 0;
	}

	// O(n) on purpose: this exists to catch a double free during testing, not
	// to be fast. It is compiled out entirely in a release build.
	bool is_free(const Block* const b) const noexcept
	{
		for (const Block* f = m_free; f != nullptr;
		     f = link_of(const_cast<Block*>(f))) {
			if (f == b) {
				return true;
			}
		}
		return false;
	}
#endif

	Block  m_blocks[BlockCount]{};
	Block* m_free = nullptr;
	Stats  m_stats{};
};

#endif /* COBS_FIXED_POOL_ALLOCATOR_H_ */
