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
#include "FixedPoolAllocator.h"
#include "PacketRef.h"

#include <cstddef>
#include <cstdint>
#include <span>

// Blocks in the default pool. Four lets the application hold a couple of
// packets without stalling reception, and is the knob most likely to want
// raising: every retained packet costs one block (§6, back-pressure).
#ifndef COBS_RX_DEFAULT_BLOCKS
#define COBS_RX_DEFAULT_BLOCKS 4u
#endif

// MaxDecodedSize comes first, and not only because it reads in protocol order
// ("a receiver for 1024-byte frames, backed by this pool") or because the rest
// of this codebase puts the shape before the mechanism. It is the only
// ordering that can carry the default below: a template parameter with a
// default may not precede one without, and MaxDecodedSize has no sensible
// default while the allocator does.
//
// PLACEHOLDER shape. The settled contract (COBS_ENGINE.md §4.3, §9.2) is
// Cobs<Allocator>: one template parameter, with the sizes coming FROM the
// policy as Allocator::rx_max_size, and CobsHeapAllocator as the default.
// This two-parameter form and its fixed-pool default stand in only until
// those policies exist, and the assembled Cobs replaces both. Do not read the
// pool default as an argument against the heap one — a pool default would
// force the library to invent a block count for the user, and every object
// would carry that quota whatever its workload.
//
//     FixedPoolAllocator<256, 4> pool;
//     CobsRx<256> rx(pool);
//
template<std::size_t MaxDecodedSize,
         class Allocator = FixedPoolAllocator<MaxDecodedSize, COBS_RX_DEFAULT_BLOCKS>>
class CobsRx final {
public:
	// Exposed so that a user who takes the default can still name the pool
	// they must construct — without it the default would save nothing:
	//     CobsRx<256>::AllocatorType pool;
	//     CobsRx<256> rx(pool);
	using AllocatorType = Allocator;
	using Packet = typename Allocator::Packet;
	using Ref = PacketRef<Allocator>;

	static_assert(Allocator::payload_capacity >= MaxDecodedSize,
		"the allocator cannot hold a maximum-size frame: the protocol states "
		"the requirement, the allocator must satisfy it");
	static_assert(MaxDecodedSize <= UINT16_MAX,
		"RxPacket::size is a uint16_t");

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
			m_allocator.deallocate(m_building);
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
			m_allocator.deallocate(m_building);
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
		m_building = m_allocator.allocate();
		if (m_building == nullptr) {
			++m_stats.allocation_failure;
			++m_stats.frames_lost;
			++m_stats.resyncs;
			m_decoder.discard_until_delimiter();
			return;
		}
		// first(MaxDecodedSize), not the whole payload: a pool with room to
		// spare must not quietly widen what the protocol accepts. The limit
		// is the protocol's, and the allocator merely has to be big enough.
		m_decoder.attach_output(m_building->writable_payload().first(MaxDecodedSize));
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
			m_allocator.deallocate(m_building);
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
