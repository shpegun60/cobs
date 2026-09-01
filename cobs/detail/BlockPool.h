/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * cobs::detail::BlockPool — fixed blocks in static storage, and nothing
 * else. No heap, no virtuals, no mutex, O(1) acquire. Checked release is
 * O(BlockCount); see COBS_POOL_CHECKS below.
 *
 * INTERNAL. This is an implementation primitive, not a settled contract for
 * user-supplied storage: it lives in detail/ so that nobody discovers it,
 * builds on it, and turns yesterday's idea into an international treaty. The
 * public shape of custom memory is decided once both real use cases exist
 * (RX packets and TX wire blocks), not before.
 *
 * Two sizes, deliberately distinct:
 *
 *   block_size    what the CLIENT asked for and sees
 *   storage_size  what the pool actually spends per block
 *
 * They differ because a FREE block stores the free-list link inside itself.
 * A two-byte TX block cannot hold an eight-byte pointer, so the pool widens
 * its own storage and raises its own alignment to fit one. That is the pool's
 * bookkeeping, not the client's data: a caller asking for byte-aligned
 * two-byte blocks is not wrong, it just has no say in how the free list is
 * threaded.
 */

#ifndef COBS_DETAIL_BLOCK_POOL_H_
#define COBS_DETAIL_BLOCK_POOL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

/*
 * Double-free and foreign-pointer detection. ON unless explicitly switched
 * off, and deliberately NOT tied to NDEBUG.
 *
 * It used to default to off in release builds, which meant the shipped
 * configuration had different safety semantics from the tested one: the
 * comments and the suite both promise that a rejected free is counted and
 * ignored, while a release build would have threaded the same block onto the
 * free list twice and corrupted it. A guarantee that evaporates under -DNDEBUG
 * is not a guarantee, it is a debugging aid with good manners.
 *
 * The cost is a walk of the free list on release — O(blocks), where blocks
 * is typically 2 to 8, on a path that is never in an ISR. Anyone who has
 * measured it and wants the bytes back sets COBS_POOL_CHECKS=0 on purpose.
 */
#ifndef COBS_POOL_CHECKS
#	define COBS_POOL_CHECKS 1
#endif

namespace cobs::detail {

// Outside the template on purpose: the numbers do not depend on the geometry,
// so two pools of different shapes report the same TYPE. Nested, every
// instantiation would have its own incompatible Stats and a policy owning an
// RX and a TX pool could not expose both through one signature.
struct PoolStats {
	uint32_t in_use     = 0;
	uint32_t high_water = 0;
	uint32_t exhausted  = 0; // dry acquire count, modulo 2^32
	uint32_t rejected   = 0; // refused release count, modulo 2^32
};

template<std::size_t BlockSize, std::size_t BlockCount, std::size_t RequestedAlignment>
class BlockPool final {
	static_assert(BlockCount >= 1, "a pool needs at least one block");
	static_assert(BlockCount <= UINT32_MAX,
		"PoolStats::in_use cannot represent this many live blocks");
	static_assert(RequestedAlignment >= 1, "alignment must be positive");
	static_assert((RequestedAlignment & (RequestedAlignment - 1u)) == 0u,
		"alignment must be a power of two");

	// Forward-declared so the free-list link's size and alignment are known
	// before the block that has to accommodate them is defined.
	struct Block;

	static constexpr std::size_t kMinStorage =
		(BlockSize > sizeof(Block*)) ? BlockSize : sizeof(Block*);
	static constexpr std::size_t kMinAlignment =
		(RequestedAlignment > alignof(Block*)) ? RequestedAlignment : alignof(Block*);

	struct alignas(kMinAlignment) Block {
		std::byte bytes[kMinStorage];
	};

public:
	// What the client asked for; storage_size is what it costs.
	static constexpr std::size_t block_size  = BlockSize;
	static constexpr std::size_t block_count = BlockCount;
	static constexpr std::size_t storage_size = sizeof(Block);

	using Stats = PoolStats;

	BlockPool() noexcept
	{
		for (std::size_t i = BlockCount; i > 0; --i) {
			Block* const b = &m_blocks[i - 1];
			set_next(b, m_free);
			m_free = b;
		}
	}

	BlockPool(const BlockPool&) = delete;
	BlockPool& operator=(const BlockPool&) = delete;

	// Raw block, or nullptr when the pool is dry. Never blocks.
	[[nodiscard]] std::byte* acquire() noexcept
	{
		Block* const b = m_free;
		if (b == nullptr) {
			++m_stats.exhausted;
			return nullptr;
		}
		m_free = next_of(b);

		++m_stats.in_use;
		if (m_stats.in_use > m_stats.high_water) {
			m_stats.high_water = m_stats.in_use;
		}
		return b->bytes;
	}

	/*
	 * Returns a block, running `cleanup(ptr)` on it first — but ONLY after
	 * the pointer has been validated.
	 *
	 * That ordering is the whole reason this takes a callback instead of the
	 * caller destroying its object and then calling release(). A foreign
	 * or already-freed pointer must be refused BEFORE anything runs a
	 * destructor on memory that may belong to somebody else.
	 *
	 * Returns false if the block was refused; the cleanup did not run.
	 */
	template<class Cleanup>
	bool release(std::byte* const ptr, Cleanup&& cleanup) noexcept
	{
		if (ptr == nullptr) {
			return false; // a harmless no-op, not an abuse: not counted
		}
		Block* const b = block_of(ptr);
#if COBS_POOL_CHECKS
		if (!owns(b) || is_free(b)) {
			++m_stats.rejected; // foreign, misaligned or already free
			return false;
		}
#endif
		std::forward<Cleanup>(cleanup)(ptr);

		set_next(b, m_free);
		m_free = b;
		--m_stats.in_use;
		return true;
	}

	// For blocks that are plain bytes with nothing to tear down.
	void release(std::byte* const ptr) noexcept
	{
		(void)release(ptr, [](std::byte*) noexcept {});
	}

	[[nodiscard]] std::size_t available() const noexcept
	{
		return BlockCount - m_stats.in_use;
	}
	[[nodiscard]] const Stats& stats() const noexcept { return m_stats; }

private:
	// The link lives in the block's own storage while the block is free, so
	// it can never alias live data. Its lifetime is started properly rather
	// than assumed: the alignment above guarantees the placement is legal.
	static void set_next(Block* const b, Block* const next) noexcept
	{
		std::construct_at(reinterpret_cast<Block**>(b->bytes), next);
	}
	static Block* next_of(const Block* const b) noexcept
	{
		return *std::launder(reinterpret_cast<Block* const*>(b->bytes));
	}

	static Block* block_of(std::byte* const ptr) noexcept
	{
		return reinterpret_cast<Block*>(ptr);
	}

#if COBS_POOL_CHECKS
	// Inside this pool, and exactly on a block boundary — a pointer into the
	// middle of a block is as wrong as one from somewhere else.
	//
	// Done on integer addresses: relational comparison and subtraction of
	// pointers into DIFFERENT objects have no portable meaning, and a foreign
	// pointer is precisely the case this exists to catch. Here uintptr_t is
	// not a dodge — the question really is about an address.
	bool owns(const Block* const b) const noexcept
	{
		const auto address = reinterpret_cast<std::uintptr_t>(b);
		const auto begin   = reinterpret_cast<std::uintptr_t>(&m_blocks[0]);
		const auto end     = begin + sizeof(m_blocks);

		if (address < begin || address >= end) {
			return false;
		}
		return ((address - begin) % sizeof(Block)) == 0u;
	}

	// O(n) on purpose: this catches a double free in every configuration where
	// checks are enabled. Defining COBS_POOL_CHECKS=0 is the explicit opt-out.
	bool is_free(const Block* const b) const noexcept
	{
		for (const Block* f = m_free; f != nullptr; f = next_of(f)) {
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

} // namespace cobs::detail

#endif /* COBS_DETAIL_BLOCK_POOL_H_ */
