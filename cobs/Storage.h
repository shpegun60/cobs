/*
 * cobs storage extension surface.
 *
 * Contract: doc/STORAGE.md. The checked concept below covers syntax; the
 * document and shared conformance suite cover its runtime obligations.
 *
 * This header exports every type needed to provide memory to the engine:
 *
 *     Format<> / Format<N> / Format<Rx, Tx>  protocol geometry
 *     RxBlock<Storage>                       typed receive ownership block
 *     TxBlock                                transmit ownership descriptor
 *     Storage                                checked C++20 contract
 *     Heap / Pool<RxN, TxN, WireFormat>      built-in strategies
 *
 * Protocol and memory stay separate. A storage implementation names one
 * Format, but never duplicates its limits. RX and TX remain intentionally
 * asymmetric: RX returns a constructed typed block followed by payload bytes;
 * TX returns raw memory together with the exact payload capacity that must
 * travel with it until release.
 */

#ifndef COBS_STORAGE_H_
#define COBS_STORAGE_H_

#include "Codec.h"
#include "Format.h"
#include "detail/BlockPool.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>

namespace cobs {

template<class StorageT>
class Packet;

} // namespace cobs

namespace cobs::detail {

template<class StorageT>
class Receiver;

} // namespace cobs::detail

namespace cobs {

/*
 * One TX ownership record. `capacity` is expressed in application payload
 * bytes, not raw allocation bytes. Storage promises that `memory` holds at
 * least Format::tx_storage_size_for_capacity(capacity) bytes.
 *
 * The descriptor moves as a unit from Storage to Message to Endpoint and back
 * to Storage. A release boundary that can lose the reported capacity is no
 * longer expressible.
 */
struct TxBlock final {
	std::byte*  memory   = nullptr;
	std::size_t capacity = 0;
};

/*
 * A decoded frame header followed immediately by its payload in the same
 * allocation. Application code only gets the immutable data() view; the
 * receiver owns decoded size and queue linkage, while Packet owns reference
 * arithmetic. Storage constructs and destroys the block but does not mutate
 * its lifetime metadata.
 */
template<class StorageT>
struct RxBlock final {
	friend class detail::Receiver<StorageT>;
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

/*
 * Syntax and exception contract for a COBS memory strategy. Behavioural
 * obligations (bounds, non-overlap, exact release, independent quotas and
 * exhaustion) are verified by the shared storage conformance suite.
 */
template<class T>
concept Storage = requires(
	T& storage,
	const std::size_t size,
	typename T::RxBlock* rx,
	TxBlock tx)
{
	typename T::Format;
	typename T::RxBlock;
	requires std::same_as<typename T::RxBlock, cobs::RxBlock<T>>;

	{ T::Format::max_receive_size } -> std::convertible_to<std::size_t>;
	{ T::Format::max_send_size } -> std::convertible_to<std::size_t>;

	{ storage.acquire_rx(size) }
		noexcept -> std::same_as<typename T::RxBlock*>;

	{ storage.release_rx(rx) }
		noexcept -> std::same_as<void>;

	{ storage.acquire_tx(size) }
		noexcept -> std::same_as<TxBlock>;

	{ storage.release_tx(tx) }
		noexcept -> std::same_as<void>;
};

/*
 * Dynamic strategy. RX allocations are exact; TX reports exactly the
 * requested payload capacity. The type is stateless and remains the default
 * storage for Endpoint.
 */
template<class WireFormat = Format<>>
class Heap final {
public:
	using Format = WireFormat;
	using RxBlock = cobs::RxBlock<Heap>;

	static_assert(cobs::codec::size_arithmetic_fits(Format::max_send_size),
		"max_send_size is too large for COBS size arithmetic");
	static_assert(Format::max_receive_size <=
		static_cast<std::size_t>(-1) - sizeof(RxBlock),
		"max_receive_size plus an RX block header overflows size_t");

	Heap() noexcept = default;
	Heap(const Heap&) = delete;
	Heap& operator=(const Heap&) = delete;

	[[nodiscard]] RxBlock* acquire_rx(
		const std::size_t requested_size) noexcept
	{
		if (requested_size > Format::max_receive_size) {
			return nullptr;
		}
		void* const memory =
			::operator new(sizeof(RxBlock) + requested_size, std::nothrow);
		if (memory == nullptr) {
			return nullptr;
		}
		return std::construct_at(static_cast<RxBlock*>(memory));
	}

	void release_rx(RxBlock* const block) noexcept
	{
		if (block == nullptr) {
			return;
		}
		std::destroy_at(block);
		::operator delete(static_cast<void*>(block));
	}

	[[nodiscard]] TxBlock acquire_tx(const std::size_t requested) noexcept
	{
		if (requested > Format::max_send_size) {
			return {};
		}
		void* const memory =
			::operator new(Format::tx_storage_size_for_capacity(requested),
			               std::nothrow);
		if (memory == nullptr) {
			return {};
		}
		return {static_cast<std::byte*>(memory), requested};
	}

	void release_tx(const TxBlock block) noexcept
	{
		::operator delete(static_cast<void*>(block.memory));
	}
};

/*
 * Deterministic strategy. RX and TX use independent fixed pools, preserving
 * independent quotas and failure accounting. Every successful TX acquisition
 * reports the full slab capacity because that capacity is already paid for.
 */
template<std::size_t RxBlocks, std::size_t TxBlocks, class WireFormat = Format<>>
class Pool final {
public:
	using Format = WireFormat;
	using RxBlock = cobs::RxBlock<Pool>;

	static constexpr std::size_t rx_blocks = RxBlocks;
	static constexpr std::size_t tx_blocks = TxBlocks;

	static_assert(cobs::codec::size_arithmetic_fits(Format::max_send_size),
		"max_send_size is too large for COBS size arithmetic");
	static_assert(Format::max_receive_size <=
		static_cast<std::size_t>(-1) - sizeof(RxBlock),
		"max_receive_size plus an RX block header overflows size_t");

private:
	using RxPool = cobs::detail::BlockPool<
		sizeof(RxBlock) + Format::max_receive_size,
		RxBlocks,
		alignof(RxBlock)>;
	using TxPool = cobs::detail::BlockPool<
		Format::tx_storage_size_for_capacity(Format::max_send_size),
		TxBlocks,
		1>;

public:
	static_assert(RxPool::block_size >=
		sizeof(RxBlock) + Format::max_receive_size,
		"the RX pool cannot hold its block header and maximum payload");
	static_assert(TxPool::block_size >=
		Format::tx_storage_size_for_capacity(Format::max_send_size),
		"the TX pool cannot hold its maximum wire frame");

	using Stats = cobs::detail::PoolStats;

	Pool() noexcept = default;
	Pool(const Pool&) = delete;
	Pool& operator=(const Pool&) = delete;

	[[nodiscard]] RxBlock* acquire_rx(
		const std::size_t requested_size) noexcept
	{
		if (requested_size > Format::max_receive_size) {
			return nullptr;
		}
		std::byte* const memory = m_rx.acquire();
		if (memory == nullptr) {
			return nullptr;
		}
		return std::construct_at(reinterpret_cast<RxBlock*>(memory));
	}

	void release_rx(RxBlock* const block) noexcept
	{
		(void)m_rx.release(reinterpret_cast<std::byte*>(block),
			[](std::byte* const memory) noexcept {
				std::destroy_at(reinterpret_cast<RxBlock*>(memory));
			});
	}

	[[nodiscard]] TxBlock acquire_tx(const std::size_t requested) noexcept
	{
		if (requested > Format::max_send_size) {
			return {};
		}
		std::byte* const memory = m_tx.acquire();
		if (memory == nullptr) {
			return {};
		}
		return {memory, Format::max_send_size};
	}

	void release_tx(const TxBlock block) noexcept
	{
		m_tx.release(block.memory);
	}

	[[nodiscard]] std::size_t rx_available() const noexcept { return m_rx.available(); }
	[[nodiscard]] std::size_t tx_available() const noexcept { return m_tx.available(); }
	[[nodiscard]] const Stats& rx_stats() const noexcept { return m_rx.stats(); }
	[[nodiscard]] const Stats& tx_stats() const noexcept { return m_tx.stats(); }

private:
	RxPool m_rx{};
	TxPool m_tx{};
};

static_assert(Storage<Heap<>>);
static_assert(std::same_as<typename Heap<>::Format, Format<>>);
static_assert(Storage<Pool<1, 1>>);
static_assert(std::same_as<typename Pool<1, 1>::Format, Format<>>);

} // namespace cobs

#endif /* COBS_STORAGE_H_ */
