/*
 * RxPacket — a decoded COBS frame, living inside one allocator block.
 *
 * Contract: doc/COBS_ENGINE.md §6. The payload is not a separate object: it
 * occupies the bytes immediately after this header, in the same block, so a
 * packet costs exactly one allocation and the decoder writes straight into
 * its final home.
 *
 * Immutability, precisely (§6.5): the decoded payload bytes are immutable
 * after publication; the ownership and queue metadata stay mutable internally.
 * That is why `refs` and `next_ready` are not const-qualified and why the
 * application only ever sees data() through a const span.
 *
 * NOTHING here is public but data(). The fields are not hidden for tidiness:
 * `size` is the length data() reports, and since RX allocations became exact
 * a packet built for 20 bytes has 20 bytes of block behind it. A public `size`
 * is therefore a one-line out-of-bounds READ —
 *
 *     auto* p = pool.allocate_rx(20);
 *     p->size = 1024;
 *     const auto byte = p->data()[1000];   // 980 bytes past the block
 *
 * — the exact counterpart of the writable_payload() hole, and closed the same
 * way. `refs`, `next_ready` and `owner` are the ownership bookkeeping the
 * whole lifetime model rests on; code that can write them can free a live
 * packet or leak a dead one. Three friends can, and each has to:
 *
 *     CobsRx      builds the packet, stamps `owner`, and threads it onto
 *                 the ready queue
 *     PacketRef   is the refcount
 *
 * The ALLOCATOR is deliberately not among them. It supplies memory and takes
 * it back; establishing ownership is the RX vertical's job. That is not a
 * stylistic preference — see CobsRx::allocate_packet().
 *
 * Allocator is used only as a pointer here, so it may be incomplete at the
 * point this template is instantiated. That is what breaks the cycle between
 * the allocator (which derives payload_capacity from sizeof(RxPacket)) and
 * the packet (which names the allocator).
 */

#ifndef COBS_RX_PACKET_H_
#define COBS_RX_PACKET_H_

#include <cstddef>
#include <cstdint>
#include <span>

// The only code allowed to write into a packet: it is also the only code that
// knows how big the block actually is.
template<class Allocator>
class CobsRx;

template<class Allocator>
class PacketRef;

template<class Allocator>
struct RxPacket final {
	friend class CobsRx<Allocator>;
	friend class PacketRef<Allocator>;

	// What the application sees: the decoded bytes, read-only. The only
	// public member, and reachable in practice through PacketRef.
	[[nodiscard]] std::span<const uint8_t> data() const noexcept
	{
		return {payload(), size};
	}

private:
	uint16_t  refs       = 1;       // the creator holds the first reference
	uint16_t  size       = 0;       // decoded bytes; set on FrameComplete
	RxPacket* next_ready = nullptr; // intrusive ready-queue link (§6.2)
	Allocator* owner     = nullptr; // who reclaims this block

	/*
	 * Where the decoder writes: exactly the declared body length this packet
	 * was allocated for, never rx_max_size.
	 *
	 * PRIVATE, and that is not tidiness. Since the length prefix arrived, a
	 * heap policy allocates 20 bytes for a 20-byte frame, so the size passed
	 * here is the ONLY thing standing between the decoder and the end of the
	 * block — writable_payload(rx_max_size) on such a packet would hand out a
	 * span running a kilobyte past its allocation, and nothing in the type
	 * could notice. The one caller that knows the right number is CobsRx,
	 * which allocated with it moments earlier, so it is the one caller that
	 * can reach this.
	 */
	[[nodiscard]] std::span<uint8_t> writable_payload(const std::size_t allocated) noexcept
	{
		return {payload(), allocated};
	}

	// The payload begins immediately after this header, inside the same
	// block. uint8_t has alignment 1, so no padding is needed or introduced.
	[[nodiscard]] uint8_t* payload() noexcept
	{
		return reinterpret_cast<uint8_t*>(this) + sizeof(RxPacket);
	}
	[[nodiscard]] const uint8_t* payload() const noexcept
	{
		return reinterpret_cast<const uint8_t*>(this) + sizeof(RxPacket);
	}
};

#endif /* COBS_RX_PACKET_H_ */
