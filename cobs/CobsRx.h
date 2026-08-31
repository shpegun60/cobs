/*
 * CobsRx — the RX vertical, assembled.
 *
 * Contract: doc/COBS_ENGINE.md §5–§7. Everything hard already lives one layer
 * down; this is the glue that answers the decoder's NeedOutput from an
 * allocator, threads completed packets onto the intrusive ready queue, and
 * hands their references to the application.
 *
 * The ownership invariant of the whole normal path (§6.3):
 *
 *     allocate()  ->  refs = 1
 *          |          held by m_building
 *          v
 *     FrameComplete   the SAME reference moves to the ready queue
 *          |
 *          v
 *     pop_packet()    the SAME reference moves into a PacketRef
 *
 * Not one refs++ or refs-- happens along it: the reference moves logically
 * rather than arithmetically, and only a copy of a PacketRef ever changes the
 * count.
 *
 * This class is the only legitimate source of packet references, which is why
 * PacketRef::adopt() is private and befriended here.
 */

#ifndef COBS_RX_H_
#define COBS_RX_H_

#include "CobsDecoder.h"
#include "CobsHeapAllocator.h"
#include "PacketRef.h"

#include <cstddef>
#include <cstdint>
#include <span>

// One template parameter, per the frozen contract (COBS_ENGINE.md §4.3): the
// policy is the single source of truth for the limits, so there is nothing
// else for a parameter to carry.
//
// This is the RX half on its own, taking the policy by reference. The
// assembled Cobs owns its policy by value (§9.4) and wraps this; an
// application normally uses Cobs rather than instantiating CobsRx directly.
template<class Allocator = CobsHeapAllocator<>>
class CobsRx final {
public:
	// Exposed so a user taking the default can still name the policy they must
	// construct — without it the default would save nothing:
	//     CobsRx<>::AllocatorType allocator;
	//     CobsRx<> rx(allocator);
	using AllocatorType = Allocator;
	using Packet = typename Allocator::Packet;
	using Ref = PacketRef<Allocator>;

	// The protocol limit comes FROM the policy (§9.2).
	static constexpr std::size_t max_decoded_size = Allocator::rx_max_size;
	static_assert(max_decoded_size <= UINT16_MAX, "RxPacket::size is a uint16_t");

	struct Stats {
		uint32_t frames_delivered  = 0;
		uint32_t frames_lost       = 0; // gaps, malformed, oversize, no memory
		uint32_t allocation_failure = 0;
		uint32_t malformed         = 0;
		uint32_t oversize          = 0;
		uint32_t resyncs           = 0; // times we had to hunt for a delimiter
	};

	explicit CobsRx(Allocator& allocator) noexcept : m_allocator(allocator) {}

	// Owns references, so it must not be copied.
	CobsRx(const CobsRx&) = delete;
	CobsRx& operator=(const CobsRx&) = delete;

	~CobsRx()
	{
		if (m_building != nullptr) {
			m_allocator.deallocate_rx(m_building);
		}
		// Release each queued reference through the same path a PacketRef
		// uses, rather than duplicating the count logic here.
		while (m_readyHead != nullptr) {
			Packet* const p = m_readyHead;
			m_readyHead = p->next_ready;
			p->next_ready = nullptr;
			(void)Ref::adopt(p); // the temporary's destructor releases it
		}
		m_readyTail = nullptr;
	}

	// Feed whatever the transport delivered. The span need not contain whole
	// frames, and may contain several.
	void consume(std::span<const uint8_t> bytes) noexcept
	{
		while (!bytes.empty()) {
			const CobsDecoder::Result r = m_decoder.consume(bytes);
			bytes = bytes.subspan(r.consumed);

			// Progress is guaranteed even when r.consumed is 0: the only such
			// case is an event that changes the decoder's state (NeedOutput,
			// which we answer below, or Oversize, which has already switched
			// to discarding).
			switch (r.event) {
			case CobsDecoder::Event::None:
				return;
			case CobsDecoder::Event::NeedOutput:
				onNeedOutput();
				break;
			case CobsDecoder::Event::FrameComplete:
				onFrameComplete(r.decoded_size);
				break;
			case CobsDecoder::Event::Malformed:
				++m_stats.malformed;
				// The delimiter that exposed it already resynchronized the
				// stream (§5.4), so this costs a frame but no resync.
				dropBuilding();
				break;
			case CobsDecoder::Event::Oversize:
				++m_stats.oversize;
				dropBuilding();
				++m_stats.resyncs;
				break;
			}
		}
	}

	// The transport lost bytes. §7: absorbed entirely here — the application
	// is never told, it simply does not receive that packet.
	void gap() noexcept
	{
		if (m_building != nullptr) {
			m_allocator.deallocate_rx(m_building);
			m_building = nullptr;
		}
		// Counted unconditionally: a gap always destroys framing continuity,
		// so at least the frame spanning it is gone whether or not one had
		// started from our side of the loss.
		++m_stats.frames_lost;
		++m_stats.resyncs;
		m_decoder.discard_until_delimiter();
		// The ready queue is untouched: everything in it structurally
		// predates the loss.
	}

	[[nodiscard]] Ref pop_packet() noexcept
	{
		Packet* const p = m_readyHead;
		if (p == nullptr) {
			return Ref{};
		}
		m_readyHead = p->next_ready;
		if (m_readyHead == nullptr) {
			m_readyTail = nullptr;
		}
		p->next_ready = nullptr;
		return Ref::adopt(p); // the queue's reference becomes the caller's
	}

	[[nodiscard]] bool has_packet() const noexcept { return m_readyHead != nullptr; }
	[[nodiscard]] const Stats& stats() const noexcept { return m_stats; }

private:
	void onNeedOutput() noexcept
	{
		m_building = m_allocator.allocate_rx();
		if (m_building == nullptr) {
			++m_stats.allocation_failure;
			++m_stats.frames_lost;
			++m_stats.resyncs;
			m_decoder.discard_until_delimiter();
			return;
		}
		// No clamp: writable_payload() is defined by the policy's declared
		// rx_max_size, so it already IS the protocol limit (§9.1.2). A policy
		// whose storage is smaller than it declares is simply broken, and the
		// place that catches it is the policy's own static_assert.
		m_decoder.attach_output(m_building->writable_payload());
	}

	void onFrameComplete(const std::size_t decoded_size) noexcept
	{
		if (m_building == nullptr) {
			// Unreachable through the decoder contract: a completion can only
			// follow a NeedOutput that was answered. Guarded anyway so a
			// broken contract costs a frame instead of a null dereference.
			return;
		}
		m_building->size = static_cast<uint16_t>(decoded_size);
		m_building->next_ready = nullptr;

		if (m_readyTail != nullptr) {
			m_readyTail->next_ready = m_building;
		} else {
			m_readyHead = m_building;
		}
		m_readyTail = m_building;
		m_building = nullptr;
		++m_stats.frames_delivered;
	}

	void dropBuilding() noexcept
	{
		if (m_building != nullptr) {
			m_allocator.deallocate_rx(m_building);
			m_building = nullptr;
		}
		++m_stats.frames_lost;
	}

	CobsDecoder m_decoder{};
	Allocator&  m_allocator;

	Packet* m_building  = nullptr;
	Packet* m_readyHead = nullptr;
	Packet* m_readyTail = nullptr;
	Stats   m_stats{};
};

#endif /* COBS_RX_H_ */
