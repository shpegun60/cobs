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
#include "CobsFrameFormat.h"
#include "CobsHeapAllocator.h"
#include "PacketRef.h"

#include <array>
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

	/*
	 * The largest BODY this instance accepts, from the policy (§9.2). Not the
	 * largest decoded frame — that is length_size bytes more, which is why
	 * this is no longer called max_decoded_size: that name was one header away
	 * from the truth, on a layer where being one header out is the easiest
	 * mistake there is.
	 */
	static constexpr std::size_t max_receive_size = Allocator::rx_max_size;
	static_assert(max_receive_size <= UINT16_MAX, "RxPacket::size is a uint16_t");

	using Format = CobsFormatFor<Allocator>;
	static constexpr std::size_t length_size = Format::length_size;

	struct Stats {
		uint32_t frames_delivered   = 0;
		uint32_t frames_lost        = 0; // every frame that did not reach the queue
		uint32_t allocation_failure = 0;
		uint32_t malformed          = 0; // structural COBS error
		uint32_t oversize           = 0; // declared length above rx_max_size
		uint32_t length_mismatch    = 0; // header absent/short, or body != declared
		uint32_t resyncs            = 0; // times we had to hunt for a delimiter
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

			// Progress is guaranteed even when r.consumed is 0. That happens
			// only on NeedOutput, where the byte still needing room is left
			// deliberately unconsumed — and onNeedOutput() either attaches the
			// next segment or switches the decoder to discarding, so the state
			// changes either way.
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
			}
		}
	}

	// The transport lost bytes. §7: absorbed entirely here — the application
	// is never told, it simply does not receive that packet.
	void gap() noexcept
	{
		releaseBuilding();
		// Counted unconditionally: a gap always destroys framing continuity,
		// so at least the frame spanning it is gone whether or not one had
		// started from our side of the loss.
		++m_stats.frames_lost;
		++m_stats.resyncs;
		m_decoder.discard_until_delimiter();
		resetFrame();
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
	/*
	 * Which segment the decoder is currently filling. This is the whole
	 * two-stage RX: the header is decoded into a couple of local bytes, and
	 * only then — knowing exactly how many body bytes are coming — is a packet
	 * allocated and the decoder pointed straight at its final home.
	 *
	 *     Header  ->  parse N  ->  allocate_rx(N)  ->  Body  ->  publish
	 *
	 * There is no staging buffer for the payload and no copy after allocation.
	 * The only temporary decoded storage in the whole RX path is the one or
	 * two bytes of m_lengthBytes.
	 */
	enum class Stage : uint8_t { Header, Body };

	void onNeedOutput() noexcept
	{
		if (m_stage == Stage::Header) {
			if (!m_headerAttached) {
				// First request of a new frame: give it the length field.
				m_headerAttached = true;
				m_decoder.attach_output(std::span<uint8_t>{m_lengthBytes});
				return;
			}
			// The header is complete and MORE decoded bytes are coming, so the
			// body starts now and its size is already known.
			beginBody();
			return;
		}

		// Stage::Body, and the packet segment is full: the frame carries more
		// body bytes than it declared.
		++m_stats.length_mismatch;
		abandonFrame();
	}

	/*
	 * The only place a packet is allocated, and the only place `owner` is set.
	 *
	 * The policy does NOT stamp it, deliberately. §9 says a policy is two
	 * constants and four functions; if it also had to write a private field of
	 * a type it merely allocates storage for, that would be a hidden fifth
	 * obligation, invisible in the signatures and impossible for the contract
	 * test to check now that the field is private. A policy written to the
	 * letter of the contract would then hand back a packet whose owner is
	 * null, and the first PacketRef release would dereference it.
	 *
	 * So the RX vertical establishes ownership, which is also where it
	 * belongs: the allocator supplies memory and takes it back, and nothing
	 * else.
	 */
	[[nodiscard]] Packet* allocate_packet(const std::size_t size) noexcept
	{
		Packet* const packet = m_allocator.allocate_rx(size);
		if (packet != nullptr) {
			packet->owner = &m_allocator;
		}
		return packet;
	}

	// Turns a complete header into an allocated packet, or refuses the frame.
	void beginBody() noexcept
	{
		const std::size_t declared = Format::load_length(m_lengthBytes.data());

		if (declared > max_receive_size) {
			++m_stats.oversize;
			abandonFrame();
			return;
		}
		if (declared == 0u) {
			// A zero-length body followed by more decoded bytes: the frame
			// contradicts its own header.
			++m_stats.length_mismatch;
			abandonFrame();
			return;
		}

		Packet* const packet = allocate_packet(declared);
		if (packet == nullptr) {
			++m_stats.allocation_failure;
			abandonFrame();
			return;
		}
		m_building = packet;
		m_declared = declared;
		m_stage = Stage::Body;
		// EXACTLY the declared length, never rx_max_size: the block may be
		// that small, and a longer span would both overrun it and hide an
		// over-length frame that has to be rejected.
		m_decoder.attach_output(packet->writable_payload(declared));
	}

	void onFrameComplete(const std::size_t decoded_size) noexcept
	{
		if (m_stage == Stage::Header) {
			// The delimiter arrived before any body did. Only one of these is
			// a real frame.
			if (decoded_size < length_size) {
				++m_stats.length_mismatch; // header absent or truncated
				endFrame(true);
				return;
			}
			const std::size_t declared = Format::load_length(m_lengthBytes.data());
			// Oversize is decided by the HEADER, so it has to be decided the
			// same way whether or not a body ever started. Without this the
			// classification would depend on the frame's punctuation: a frame
			// declaring 65 with one body byte is oversize, and the same frame
			// with no body at all would be a mere length mismatch.
			if (declared > max_receive_size) {
				++m_stats.oversize;
				endFrame(true); // the delimiter is already consumed: no resync
				return;
			}
			if (declared != 0u) {
				++m_stats.length_mismatch; // declared a body, sent none
				endFrame(true);
				return;
			}
			// A legitimately empty application packet.
			Packet* const packet = allocate_packet(0);
			if (packet == nullptr) {
				++m_stats.allocation_failure;
				endFrame(true);
				return;
			}
			m_building = packet;
			publish(0);
			return;
		}

		// Stage::Body. decoded_size counts the header too.
		const std::size_t body = decoded_size - length_size;
		if (body != m_declared) {
			// Short: the delimiter came before the declared bytes did. Long is
			// impossible here — the segment is exactly m_declared bytes, so an
			// extra byte raises NeedOutput instead of arriving.
			++m_stats.length_mismatch;
			endFrame(true);
			return;
		}
		publish(body);
	}

	void publish(const std::size_t body) noexcept
	{
		m_building->size = static_cast<uint16_t>(body);
		m_building->next_ready = nullptr;
		if (m_readyTail != nullptr) {
			m_readyTail->next_ready = m_building;
		} else {
			m_readyHead = m_building;
		}
		m_readyTail = m_building;
		m_building = nullptr;
		++m_stats.frames_delivered;
		endFrame(false);
	}

	// Gives up on a frame BEFORE its delimiter, so the stream still has to be
	// hunted forward — that is what makes this a resync, unlike a failure the
	// delimiter itself revealed.
	void abandonFrame() noexcept
	{
		releaseBuilding();
		++m_stats.frames_lost;
		++m_stats.resyncs;
		m_decoder.discard_until_delimiter();
		resetFrame();
	}

	// The frame is over — the delimiter has already resynchronized us, so a
	// loss here costs no resync.
	void endFrame(const bool lost) noexcept
	{
		if (lost) {
			releaseBuilding();
			++m_stats.frames_lost;
		}
		resetFrame();
	}

	void resetFrame() noexcept
	{
		m_stage = Stage::Header;
		m_headerAttached = false;
		m_declared = 0;
	}

	void releaseBuilding() noexcept
	{
		if (m_building != nullptr) {
			m_allocator.deallocate_rx(m_building);
			m_building = nullptr;
		}
	}

	void dropBuilding() noexcept
	{
		releaseBuilding();
		++m_stats.frames_lost;
		resetFrame();
	}

	CobsDecoder m_decoder{};
	Allocator&  m_allocator;

	std::array<uint8_t, Format::length_size> m_lengthBytes{};
	std::size_t m_declared = 0;
	Stage m_stage = Stage::Header;
	bool  m_headerAttached = false;

	Packet* m_building  = nullptr;
	Packet* m_readyHead = nullptr;
	Packet* m_readyTail = nullptr;
	Stats   m_stats{};
};

#endif /* COBS_RX_H_ */
