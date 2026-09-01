/*
 * cobs::detail::Receiver — the RX vertical, assembled.
 *
 * Contract: doc/COBS_ENGINE.md §5–§7. Everything hard already lives one layer
 * down; this is the glue that answers the decoder's NeedOutput from an
 * storage, threads completed blocks onto the intrusive ready queue, and
 * hands their references to the application.
 *
 * The ownership invariant of the whole normal path (§6.3):
 *
 *     acquire_rx()  ->  refs = 1
 *          |          held by m_building
 *          v
 *     FrameComplete   the SAME reference moves to the ready queue
 *          |
 *          v
 *     pop_packet()    the SAME reference moves into a Packet
 *
 * Not one refs++ or refs-- happens along it: the reference moves logically
 * rather than arithmetically, and only a copy of a Packet ever changes the
 * count.
 *
 * This class is the only legitimate source of packet references, which is why
 * Packet::adopt() is private and befriended here.
 */

#ifndef COBS_DETAIL_RECEIVER_H_
#define COBS_DETAIL_RECEIVER_H_

#include "../Codec.h"
#include "../Stats.h"
#include "Packet.h"
#include "../Storage.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cobs::detail {

// One template parameter, per the frozen contract (COBS_ENGINE.md §4.3): the
// storage names one Format, so a separate geometry parameter could only
// disagree with it.
//
// This is the RX half on its own, taking storage by reference. The assembled
// Endpoint owns storage by value (§9.4) and wraps this; an
// application uses Endpoint rather than naming Receiver directly.
template<class StorageT>
class Receiver final {
	static_assert(cobs::Storage<StorageT>,
		"Receiver storage must satisfy the cobs::Storage contract");

public:
	using StorageType = StorageT;
	using Block = typename StorageT::RxBlock;
	using Packet = cobs::Packet<StorageT>;
	using Format = typename StorageT::Format;

	/*
	 * The largest BODY this instance accepts, from Format (§9.2). Not the
	 * largest decoded frame — that is length_size bytes more, which is why
	 * this is no longer called max_decoded_size: that name was one header away
	 * from the truth, on a layer where being one header out is the easiest
	 * mistake there is.
	 */
	static constexpr std::size_t max_receive_size = Format::max_receive_size;
	static_assert(max_receive_size <= UINT16_MAX, "RxBlock::size is a uint16_t");

	static constexpr std::size_t length_size = Format::length_size;

	explicit Receiver(StorageT& storage) noexcept : m_storage(storage)
	{
		prepareHeader();
	}

	// Owns references, so it must not be copied.
	Receiver(const Receiver&) = delete;
	Receiver& operator=(const Receiver&) = delete;

	~Receiver()
	{
		releaseBuilding();
		clearReady();
	}

	// Feed whatever the transport delivered. The span need not contain whole
	// frames, and may contain several.
	void consume(std::span<const uint8_t> bytes) noexcept
	{
		while (!bytes.empty()) {
			const cobs::codec::Decoder::Result r = m_decoder.consume(bytes);
			bytes = bytes.subspan(r.consumed);

			// Progress is guaranteed even when r.consumed is 0. That happens
			// only on NeedOutput, where the byte still needing room is left
			// deliberately unconsumed — and onNeedOutput() either attaches the
			// next segment or switches the decoder to discarding, so the state
			// changes either way.
			switch (r.event) {
			case cobs::codec::Decoder::Event::None:
				return;
			case cobs::codec::Decoder::Event::NeedOutput:
				onNeedOutput();
				break;
			case cobs::codec::Decoder::Event::FrameComplete:
				onFrameComplete(r.decoded_size);
				break;
			case cobs::codec::Decoder::Event::Malformed:
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

	[[nodiscard]] Packet pop_packet() noexcept
	{
		Block* const p = dequeueReady();
		if (p == nullptr) {
			return Packet{};
		}
		return Packet::adopt(p); // the queue's reference becomes the caller's
	}

	[[nodiscard]] bool has_packet() const noexcept { return m_readyHead != nullptr; }
	[[nodiscard]] const cobs::Stats::Rx& stats() const noexcept { return m_stats; }

private:
	/*
	 * Which segment the decoder is currently filling. This is the whole
	 * two-stage RX: the header is decoded into a couple of local bytes, and
	 * only then — knowing exactly how many body bytes are coming — is a packet
	 * allocated and the decoder pointed straight at its final home.
	 *
	 *     Header  ->  parse N  ->  acquire_rx(N)  ->  Body  ->  publish
	 *
	 * There is no staging buffer for the payload and no copy after allocation.
	 * The only temporary decoded storage in the whole RX path is the one or
	 * two bytes of m_lengthBytes.
	 */
	// NeedHeader and Header are distinct states instead of a Stage plus a
	// separate boolean. The impossible combinations are therefore not
	// representable: Header always means the local segment is attached.
	enum class Stage : uint8_t { NeedHeader, Header, Body };

	void onNeedOutput() noexcept
	{
		if (m_stage == Stage::NeedHeader) {
			// First request after a resynchronization: give it the length field.
			m_stage = Stage::Header;
			m_decoder.attach_output(std::span<uint8_t>{m_lengthBytes});
			return;
		}
		if (m_stage == Stage::Header) {
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
	 * Storage does NOT stamp it, deliberately. §9 says storage names Format
	 * and RxBlock and provides four operations; if it also had to write a private field of
	 * a type it merely allocates storage for, that would be a hidden fifth
	 * obligation, invisible in the signatures and impossible for the contract
	 * test to check now that the field is private. Storage written to the
	 * letter of the contract would then hand back a packet whose owner is
	 * null, and the first Packet release would dereference it.
	 *
	 * So the RX vertical establishes ownership, which is also where it
	 * belongs: storage supplies memory and takes it back, and nothing
	 * else.
	 */
	[[nodiscard]] Block* acquire_block(const std::size_t size) noexcept
	{
		Block* const block = m_storage.acquire_rx(size);
		if (block != nullptr) {
			block->owner = &m_storage;
		}
		return block;
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

		Block* const block = acquire_block(declared);
		if (block == nullptr) {
			++m_stats.allocation_failure;
			abandonFrame();
			return;
		}
		m_building = block;
		// While the block is private to the building state, its existing size
		// field is the single source of truth for the declared body length. It
		// becomes the published logical size only after the equality check.
		m_building->size = static_cast<uint16_t>(declared);
		m_stage = Stage::Body;
		// EXACTLY the declared length, never rx_max_size: the block may be
		// that small, and a longer span would both overrun it and hide an
		// over-length frame that has to be rejected.
		m_decoder.attach_output(block->writable_payload(declared));
	}

	void onFrameComplete(const std::size_t decoded_size) noexcept
	{
		if (m_stage != Stage::Body) {
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
			Block* const block = acquire_block(0);
			if (block == nullptr) {
				++m_stats.allocation_failure;
				endFrame(true);
				return;
			}
			m_building = block;
			publish(0);
			return;
		}

		// Stage::Body. decoded_size counts the header too.
		const std::size_t body = decoded_size - length_size;
		if (body != static_cast<std::size_t>(m_building->size)) {
			// Short: the delimiter came before the declared bytes did. Long is
			// impossible here — the segment is exactly the declared bytes, so an
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
		enqueueReady(m_building);
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
		prepareHeader();
	}

	void resetFrame() noexcept
	{
		m_stage = Stage::NeedHeader;
	}

	void prepareHeader() noexcept
	{
		m_stage = Stage::Header;
		m_decoder.prepare_output(std::span<uint8_t>{m_lengthBytes});
	}

	void releaseBuilding() noexcept
	{
		if (m_building != nullptr) {
			m_storage.release_rx(m_building);
			m_building = nullptr;
		}
	}

	void dropBuilding() noexcept
	{
		releaseBuilding();
		++m_stats.frames_lost;
		resetFrame();
		prepareHeader();
	}

	void enqueueReady(Block* const block) noexcept
	{
		block->next_ready = nullptr;
		if (m_readyTail != nullptr) {
			m_readyTail->next_ready = block;
		} else {
			m_readyHead = block;
		}
		m_readyTail = block;
	}

	[[nodiscard]] Block* dequeueReady() noexcept
	{
		Block* const block = m_readyHead;
		if (block == nullptr) {
			return nullptr;
		}
		m_readyHead = block->next_ready;
		if (m_readyHead == nullptr) {
			m_readyTail = nullptr;
		}
		block->next_ready = nullptr;
		return block;
	}

	void clearReady() noexcept
	{
		while (Block* const block = dequeueReady()) {
			// Release through Packet so there is only one refcount path.
			(void)Packet::adopt(block);
		}
	}

	cobs::codec::Decoder m_decoder{};
	StorageT& m_storage;

	std::array<uint8_t, Format::length_size> m_lengthBytes{};
	Stage m_stage = Stage::NeedHeader;

	Block* m_building  = nullptr;
	Block* m_readyHead = nullptr;
	Block* m_readyTail = nullptr;
	cobs::Stats::Rx m_stats{};
};

} // namespace cobs::detail

#endif /* COBS_DETAIL_RECEIVER_H_ */
