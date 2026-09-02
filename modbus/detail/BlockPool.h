/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Fixed, intrusive-free-list storage used by Modbus Pool policies. */

#ifndef MODBUS_DETAIL_BLOCK_POOL_H_
#define MODBUS_DETAIL_BLOCK_POOL_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#ifndef MODBUS_POOL_CHECKS
#	define MODBUS_POOL_CHECKS 1
#endif

namespace modbus::detail {

struct PoolStats final {
	uint32_t in_use = 0;
	uint32_t high_water = 0;
	uint32_t exhausted = 0;
	uint32_t rejected = 0;
};

template<std::size_t BlockSize, std::size_t BlockCount, std::size_t RequestedAlignment>
class BlockPool final {
	static_assert(BlockCount >= 1u, "a pool needs at least one block");
	static_assert(BlockCount <= UINT32_MAX,
		"PoolStats::in_use cannot represent this many blocks");
	static_assert(RequestedAlignment >= 1u, "alignment must be positive");
	static_assert((RequestedAlignment & (RequestedAlignment - 1u)) == 0u,
		"alignment must be a power of two");

	struct Block;
	static constexpr std::size_t storage_bytes =
		(BlockSize > sizeof(Block*)) ? BlockSize : sizeof(Block*);
	static constexpr std::size_t storage_alignment =
		(RequestedAlignment > alignof(Block*)) ? RequestedAlignment : alignof(Block*);

	struct alignas(storage_alignment) Block final {
		std::byte bytes[storage_bytes];
	};

public:
	static constexpr std::size_t block_size = BlockSize;
	static constexpr std::size_t block_count = BlockCount;
	static constexpr std::size_t storage_size = sizeof(Block);
	using Stats = PoolStats;

	BlockPool() noexcept
	{
		for (std::size_t i = BlockCount; i > 0u; --i) {
			Block* const block = &m_blocks[i - 1u];
			set_next(block, m_free);
			m_free = block;
		}
	}

	BlockPool(const BlockPool&) = delete;
	BlockPool& operator=(const BlockPool&) = delete;

	[[nodiscard]] std::byte* acquire() noexcept
	{
		Block* const block = m_free;
		if (block == nullptr) {
			++m_stats.exhausted;
			return nullptr;
		}
		m_free = next_of(block);
		++m_stats.in_use;
		if (m_stats.in_use > m_stats.high_water) {
			m_stats.high_water = m_stats.in_use;
		}
		return block->bytes;
	}

	template<class Cleanup>
	bool release(std::byte* const ptr, Cleanup&& cleanup) noexcept
	{
		if (ptr == nullptr) {
			return false;
		}
#if MODBUS_POOL_CHECKS
		if (!owns(ptr)) {
			++m_stats.rejected;
			return false;
		}
#endif
		Block* const block = static_cast<Block*>(static_cast<void*>(ptr));
#if MODBUS_POOL_CHECKS
		if (is_free(block)) {
			++m_stats.rejected;
			return false;
		}
#endif
		std::forward<Cleanup>(cleanup)(ptr);
		set_next(block, m_free);
		m_free = block;
		--m_stats.in_use;
		return true;
	}

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
	static void set_next(Block* const block, Block* const next) noexcept
	{
		std::construct_at(
			static_cast<Block**>(static_cast<void*>(block->bytes)), next);
	}

	[[nodiscard]] static Block* next_of(const Block* const block) noexcept
	{
		return *std::launder(
			static_cast<Block* const*>(static_cast<const void*>(block->bytes)));
	}

#if MODBUS_POOL_CHECKS
	[[nodiscard]] bool owns(const std::byte* const ptr) const noexcept
	{
		const auto address = reinterpret_cast<std::uintptr_t>(ptr);
		const auto begin = reinterpret_cast<std::uintptr_t>(&m_blocks[0]);
		const auto end = begin + sizeof(m_blocks);
		return address >= begin && address < end &&
		       ((address - begin) % sizeof(Block)) == 0u;
	}

	[[nodiscard]] bool is_free(const Block* const block) const noexcept
	{
		for (const Block* item = m_free; item != nullptr; item = next_of(item)) {
			if (item == block) {
				return true;
			}
		}
		return false;
	}
#endif

	Block m_blocks[BlockCount]{};
	Block* m_free = nullptr;
	Stats m_stats{};
};

} // namespace modbus::detail

#endif /* MODBUS_DETAIL_BLOCK_POOL_H_ */
