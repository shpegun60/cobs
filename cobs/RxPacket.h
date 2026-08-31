/*
 * RxPacket — a decoded COBS frame, living inside one allocator block.
 *
 * Contract: doc/COBS_ENGINE.md §6. The payload is not a separate object: it
 * occupies the bytes immediately after this header, in the same block, so a
 * packet costs exactly one allocation and the decoder writes straight into
 * its final home.
 *
 * Immutability, precisely (§6.5): the decoded payload bytes are immutable
 * after publication; the ownership and queue metadata below stay mutable
 * internally. That is why `refs` and `next_ready` are not const-qualified and
 * why the application only ever sees data() through a const span.
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

template<class Allocator>
struct RxPacket final {
	uint16_t  refs       = 1;       // the creator holds the first reference
	uint16_t  size       = 0;       // decoded bytes; set on FrameComplete
	RxPacket* next_ready = nullptr; // intrusive ready-queue link (§6.2)
	Allocator* owner     = nullptr; // who reclaims this block

	// Where the decoder writes: exactly the protocol limit the policy
	// declared (COBS_ENGINE.md §9.1.1), never the physical region, which may
	// be larger and is nobody's business. Only the owner of an unpublished
	// packet may call this.
	[[nodiscard]] std::span<uint8_t> writable_payload() noexcept
	{
		return {payload(), Allocator::rx_max_size};
	}

	// What the application sees: the decoded bytes, read-only.
	[[nodiscard]] std::span<const uint8_t> data() const noexcept
	{
		return {payload(), size};
	}

private:
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
