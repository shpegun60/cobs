/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * wire::Storage — where a protocol's bytes come from, and nothing else.
 *
 * Contract: doc/STORAGE.md. One storage library serves every protocol in the
 * repository (COBS, Modbus RTU, whatever comes next) because storage knows
 * nothing about any of them. It sees three compile-time numbers and four
 * operations on raw bytes:
 *
 *     Geometry::rx_block_bytes   the largest RX request that will ever come
 *     Geometry::tx_block_bytes   the largest TX request that will ever come
 *     Geometry::alignment        alignment every RX block must satisfy
 *
 *     acquire_rx(bytes) -> std::byte*     >= bytes, aligned, or nullptr
 *     release_rx(memory)                  the pointer acquire_rx returned; nullptr is a no-op
 *     acquire_tx(bytes) -> TxBlock        {memory, granted >= bytes}, or {}
 *     release_tx(block)                   the descriptor acquire_tx returned; {} is a no-op
 *
 * The protocol constructs its own block header inside the RX bytes, keeps its
 * own view of the TX block's payload, and converts between "payload bytes"
 * and "physical bytes" with its own Format. Storage never sees a header type,
 * a length field or a CRC. That is what lets
 *
 *     cobs::Endpoint<wire::Pool<8, 2>, ...>
 *     modbus::rtu::Endpoint<wire::Pool<8, 2>, ...>
 *
 * name the SAME memory type, and lets a user write one custom storage for
 * both.
 *
 * ---------------------------------------------------------------------------
 * LATE BINDING. A storage is a specification with a nested template,
 *
 *     struct MyStorage {
 *         template<class Geometry> class For { ...four operations... };
 *     };
 *
 * and the endpoint instantiates `MyStorage::For<Geometry>` with the geometry
 * it computed from its Format — the endpoint is the only party that knows
 * both the protocol limits and the size of its private block header. This is
 * the allocator "rebind" idiom: the user names the strategy, the container
 * binds it to its own node. `wire::Heap` and `wire::Pool<Rx, Tx>` have the
 * same shape, so a user-written storage looks exactly like a built-in one.
 * ---------------------------------------------------------------------------
 * UNITS. Everything here is physical bytes. `TxBlock::granted` is the number
 * of bytes the block really holds, which may exceed the request (a size-class
 * pool, a single slab); the descriptor travels unchanged to release_tx(). A
 * protocol derives its payload capacity from `granted` with its own Format
 * and never edits the descriptor.
 * ---------------------------------------------------------------------------
 * ALIGNMENT. Every RX pointer a storage returns must be aligned to
 * Geometry::alignment: the protocol constructs a header of pointers in it, and
 * a Cortex-M7 faults on an unaligned LDRD/STRD rather than merely slowing
 * down. Geometry::rx_block_bytes is a multiple of Geometry::alignment (the
 * Geometry concept refuses anything else), so an array of blocks keeps every
 * slot aligned — but only if the slots are declared with the geometry's own
 * numbers. A storage that invents its own row size defeats that: with a
 * maximum of 1044, rows of 1045 pass every size check and misalign every
 * second slot. Let the type system carry the stride:
 *
 *     struct alignas(Geometry::alignment) RxSlot {
 *         std::byte bytes[Geometry::rx_block_bytes];
 *     };
 *     RxSlot slots[N];              // slot i is aligned for every i
 *
 * `wire::Pool` does exactly this inside detail::BlockPool. Heap relies on
 * ::operator new, whose default alignment covers every block header this
 * repository has. The conformance suite checks the alignment of every RX
 * grant it receives, so a custom storage that gets this wrong fails there
 * before it faults on a board.
 */

#ifndef WIRE_STORAGE_H_
#define WIRE_STORAGE_H_

#include "detail/BlockPool.h"

#include <concepts>
#include <cstddef>
#include <limits>
#include <new>
#include <type_traits>

namespace wire {

/*
 * One TX ownership record. `granted` is the PHYSICAL byte count the storage
 * really provides for `memory`; it is at least what was requested and it is
 * what comes back in release_tx(). It is not a payload capacity, a message
 * size or a frame length — the protocol derives those from it.
 */
struct TxBlock final {
	std::byte*  memory  = nullptr;
	std::size_t granted = 0;
};

[[nodiscard]] constexpr std::size_t round_up(
		const std::size_t bytes, const std::size_t alignment) noexcept
{
	// Zero is an invalid geometry sentinel, including arithmetic overflow.
	if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
		return 0u;
	}
	const std::size_t remainder = bytes % alignment;
	const std::size_t padding = remainder == 0u ? 0u : alignment - remainder;
	return bytes <= std::numeric_limits<std::size_t>::max() - padding
		? bytes + padding : 0u;
}

/*
 * The shape a geometry's three numbers must have, checked by the concept below
 * so that the contract documented above is the contract enforced:
 *
 *   - alignment is a power of two (one is allowed: "no alignment demand");
 *   - rx_block_bytes is a multiple of alignment, so that slot i of an array
 *     of such blocks is as aligned as slot 0 — the rounding the header
 *     comment promises, verified rather than assumed;
 *   - both block sizes are at least one byte: a zero-byte RX block could
 *     not hold a header, and a zero-byte TX block could not hold a frame.
 */
[[nodiscard]] constexpr bool geometry_shape(
		const std::size_t rx_block_bytes,
		const std::size_t tx_block_bytes,
		const std::size_t alignment) noexcept
{
	return alignment >= 1u && (alignment & (alignment - 1u)) == 0u &&
	       rx_block_bytes >= 1u && tx_block_bytes >= 1u &&
	       rx_block_bytes % alignment == 0u;
}

/*
 * What the endpoint hands to a storage specification: the maximum request in
 * each direction and the RX alignment. Nothing about what the bytes mean.
 *
 * The three members must be CONSTANT EXPRESSIONS of integral type — a fixed
 * pool sizes its slabs from them at compile time — and must have the shape
 * geometry_shape() describes. Spelling the type requirement through
 * std::integral_constant is what enforces both constness and integrality: a
 * runtime variable, or a floating-point constant, is not a valid template
 * argument there, so the concept is simply false rather than accepting a
 * geometry that a `static constexpr` context would later reject.
 */
template<class G>
concept Geometry = requires {
	typename std::integral_constant<std::size_t, G::rx_block_bytes>;
	typename std::integral_constant<std::size_t, G::tx_block_bytes>;
	typename std::integral_constant<std::size_t, G::alignment>;
} && geometry_shape(G::rx_block_bytes, G::tx_block_bytes, G::alignment);

/*
 * The geometry type endpoints actually bind: keyed on the three NUMBERS and
 * nothing else, so two endpoints whose blocks have the same shape instantiate
 * the same `Storage::For<...>`, the same storage type and therefore the same
 * Packet and Message. In particular a CRC policy choice that changes only the
 * calculator (Bitwise vs Table) leaves every one of those types identical.
 * A geometry nested inside the endpoint class would be a distinct type per
 * endpoint specialization and would silently duplicate all of them.
 */
template<std::size_t RxBlockBytes, std::size_t TxBlockBytes, std::size_t Alignment>
struct BlockGeometry final {
	static_assert(geometry_shape(RxBlockBytes, TxBlockBytes, Alignment),
		"block geometry needs positive sizes, power-of-two alignment and aligned RX stride");

	static constexpr std::size_t rx_block_bytes = RxBlockBytes;
	static constexpr std::size_t tx_block_bytes = TxBlockBytes;
	static constexpr std::size_t alignment = Alignment;
};

/*
 * The four operations, on an INSTANTIATED storage. Syntax and exception
 * guarantee only; the behavioural obligations (non-overlap, exact release,
 * independent RX/TX quotas, alignment of every returned pointer) are checked
 * by the shared conformance suite, wire/tests/test_storage.cpp.
 */
template<class S>
concept ByteStorage = requires(
	S& storage,
	const std::size_t bytes,
	std::byte* const memory,
	const TxBlock block)
{
	{ storage.acquire_rx(bytes) } noexcept -> std::same_as<std::byte*>;
	{ storage.release_rx(memory) } noexcept -> std::same_as<void>;
	{ storage.acquire_tx(bytes) } noexcept -> std::same_as<TxBlock>;
	{ storage.release_tx(block) } noexcept -> std::same_as<void>;
};

/*
 * The specification an endpoint accepts: a type whose nested `For<Geometry>`
 * is a ByteStorage. This is the concept a protocol's Endpoint asserts on its
 * memory parameter.
 */
template<class Spec, class G>
concept Storage = Geometry<G> && requires {
	typename Spec::template For<G>;
} && ByteStorage<typename Spec::template For<G>>;

/*
 * Dynamic storage: exact per-request allocations from the global heap, no
 * quota, no occupancy counters. Stateless, so an endpoint holding it with
 * [[no_unique_address]] pays no bytes for it. The default for every protocol
 * endpoint: desktop tools, tests, and systems where dynamic allocation is an
 * accepted policy.
 */
struct Heap final {
	// Constrained rather than static_asserted so that wire::Storage<Heap, G>
	// is FALSE for a geometry the heap cannot serve — one demanding more
	// alignment than ::operator new guarantees — instead of a hard error
	// inside the concept check. Both protocols' block headers are pointer-
	// aligned, so the limit is academic until somebody wants cache-line slabs;
	// wire::Pool serves those.
	template<class G>
		requires Geometry<G> && (G::alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__)
	class For final {
	public:
		using Geometry = G;

		For() noexcept = default;
		For(const For&) = delete;
		For& operator=(const For&) = delete;

		// A request beyond the geometry's maximum fails rather than being
		// silently honoured: the protocol promised never to make one, so it is
		// a bug, and the protocol counts it as an allocation failure.
		[[nodiscard]] std::byte* acquire_rx(const std::size_t bytes) noexcept
		{
			if (bytes > G::rx_block_bytes) {
				return nullptr;
			}
			return static_cast<std::byte*>(::operator new(bytes, std::nothrow));
		}

		void release_rx(std::byte* const memory) noexcept
		{
			::operator delete(static_cast<void*>(memory)); // nullptr is a no-op
		}

		[[nodiscard]] TxBlock acquire_tx(const std::size_t bytes) noexcept
		{
			if (bytes > G::tx_block_bytes) {
				return {};
			}
			void* const memory = ::operator new(bytes, std::nothrow);
			if (memory == nullptr) {
				return {};
			}
			return {static_cast<std::byte*>(memory), bytes}; // exactly what was asked
		}

		void release_tx(const TxBlock block) noexcept
		{
			::operator delete(static_cast<void*>(block.memory));
		}
	};
};

/*
 * Deterministic storage: RxBlocks and TxBlocks fixed slabs, sized by the
 * geometry the endpoint binds, in static storage inside the endpoint. O(1)
 * acquire, no heap. RX and TX are independent pools, so holding every RX
 * block never starves TX and vice versa.
 *
 * The two numbers are OWNERSHIP QUOTAS, not byte sizes: how many RX packets
 * may be alive at once (decoding, queued, retained by Packet handles) and how
 * many TX messages may be alive at once (being built, plus the one the
 * transport is reading). The slab sizes come from the geometry, so the same
 * `wire::Pool<8, 2>` costs a different number of bytes inside a COBS endpoint
 * with 1024-byte frames and inside a Modbus RTU endpoint with 256-byte ADUs.
 * Ask `sizeof(Link)`, or its `Geometry`, when the number matters.
 *
 * Every TX acquisition grants the whole slab, because the slab was paid for
 * whatever was asked; the protocol's Message turns that into payload capacity.
 */
template<std::size_t RxBlocks, std::size_t TxBlocks>
struct Pool final {
	static_assert(RxBlocks >= 1u, "a pool needs at least one RX block");
	static_assert(TxBlocks >= 1u, "a pool needs at least one TX block");

	static constexpr std::size_t rx_blocks = RxBlocks;
	static constexpr std::size_t tx_blocks = TxBlocks;

	template<class G>
	class For final {
		static_assert(Geometry<G>, "Pool must be bound to a wire::Geometry");

		using RxPool = detail::BlockPool<G::rx_block_bytes, RxBlocks, G::alignment>;
		using TxPool = detail::BlockPool<G::tx_block_bytes, TxBlocks, 1u>;

	public:
		using Geometry = G;
		using Stats = detail::PoolStats;

		static constexpr std::size_t rx_blocks = RxBlocks;
		static constexpr std::size_t tx_blocks = TxBlocks;

		For() noexcept = default;
		For(const For&) = delete;
		For& operator=(const For&) = delete;

		[[nodiscard]] std::byte* acquire_rx(const std::size_t bytes) noexcept
		{
			if (bytes > RxPool::block_size) {
				return nullptr;
			}
			return m_rx.acquire();
		}

		void release_rx(std::byte* const memory) noexcept
		{
			m_rx.release(memory);
		}

		[[nodiscard]] TxBlock acquire_tx(const std::size_t bytes) noexcept
		{
			if (bytes > TxPool::block_size) {
				return {};
			}
			std::byte* const memory = m_tx.acquire();
			if (memory == nullptr) {
				return {};
			}
			return {memory, TxPool::block_size}; // the whole slab, always
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
};

} // namespace wire

#endif /* WIRE_STORAGE_H_ */
