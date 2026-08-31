/*
 * CobsHeapAllocator — the reference implementation of the allocator policy
 * (doc/COBS_ENGINE.md §9), and the default for `Cobs<>`.
 *
 * It is the smallest thing that can satisfy the contract, which makes it the
 * example to read before writing your own: two constants, four functions, and
 * no state at all.
 *
 * The limits are template parameters rather than "unbounded" for two reasons,
 * neither of them the old one. The wire length field is fixed-width, so the
 * largest frame the format can describe is settled when the type is
 * instantiated; and a protocol needs a configured ceiling above which a
 * declared length is refused rather than believed. The PER-FRAME allocation is
 * exact — allocate_rx(N) for a frame that declared N (§9.1.1).
 */

#ifndef COBS_HEAP_ALLOCATOR_H_
#define COBS_HEAP_ALLOCATOR_H_

#include "CobsEncoder.h"
#include "CobsFrameFormat.h"
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
	using Format = CobsFrameFormat<RxMaxSize, TxMaxSize>;

	// A policy defends its own geometry at compile time (§9.1.2), and that
	// includes the arithmetic it is built on. Both of these are unreachable
	// for any sane limit and both are silent corruption if they ever hold.
	static_assert(cobs_size_arithmetic_fits(tx_max_size),
		"tx_max_size is too large for the COBS size arithmetic to stay within size_t");
	static_assert(rx_max_size <= static_cast<std::size_t>(-1) - sizeof(Packet),
		"rx_max_size plus a packet header overflows size_t");

	CobsHeapAllocator() noexcept = default;
	CobsHeapAllocator(const CobsHeapAllocator&) = delete;
	CobsHeapAllocator& operator=(const CobsHeapAllocator&) = delete;

	/*
	 * One contiguous [RxPacket][payload] region, as §9.1.2 requires, sized for
	 * EXACTLY the declared body length. That is what the wire length prefix
	 * buys: a 20-byte frame costs 20 payload bytes here, not rx_max_size.
	 *
	 * A heap policy has no trouble honouring contiguity, so heap and pool end
	 * up with identical geometry and differ only in where the region came from
	 * and how much of it is spent.
	 */
	[[nodiscard]] Packet* allocate_rx(const std::size_t requested_size) noexcept
	{
		if (requested_size > rx_max_size) {
			return nullptr;
		}
		void* const memory =
			::operator new(sizeof(Packet) + requested_size, std::nothrow);
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
	 * Exactly the capacity asked for, and a block sized for exactly that —
	 * plus the length header, which shares the block and is part of what gets
	 * encoded. This is where the sized TX contract pays: with a two-byte
	 * header, a seven-byte CAPACITY REQUEST costs 11 bytes here
	 * (cobs_max_wire_size(2 + 7)) rather than the 1032 a block for the largest
	 * frame would take. The request, not the message: a seven-byte payload
	 * built through make_msg() lives in whatever reserve its capacity hint
	 * asked for.
	 *
	 * No rounding up, no size classes, no growth rule. Deciding how much to
	 * ask for is the CONTAINER's job (§9.1.0): CobsMsg knows its current
	 * capacity and what it needs, so it computes the geometric target and asks
	 * once. A policy that also had an opinion would be two growth rules
	 * fighting over one allocation.
	 *
	 * A request for zero still gets real storage — enough for a frame whose
	 * decoded content is the length field alone — while the reported payload
	 * capacity is honestly zero.
	 */
	[[nodiscard]] TxAllocation allocate_tx(const std::size_t requested) noexcept
	{
		if (requested > tx_max_size) {
			return {};
		}
		// The block must hold the encoded [length][payload], not just the
		// payload: the header is part of the COBS input (§8.3).
		void* const memory =
			::operator new(Format::tx_storage_size_for_capacity(requested), std::nothrow);
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
