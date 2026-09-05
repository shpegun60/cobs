/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * cobs::detail::Receiver — the RX vertical, assembled.
 *
 * Contract: doc/COBS_ENGINE.md §5–§7. Everything hard already lives one layer
 * down; this is the glue that answers the decoder's NeedOutput from storage,
 * threads completed blocks onto the intrusive ready queue, and hands their
 * references to the application.
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
#include "../Format.h"
#include "../Stats.h"
#include "../../wire/Storage.h"
#include "Packet.h"
#include "RxBlock.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace cobs::detail {

// Two template parameters: the instantiated storage (a supply of bytes, per
// wire/Storage.h) and the protocol Format. The storage has no opinion about
// the Format, so the endpoint names both and passes them down.
//
// This is the RX half on its own, taking storage by reference. The assembled
// Endpoint owns storage by value (§9.4) and wraps this; an application uses
// Endpoint rather than naming Receiver directly.
template<class StorageT, class LayoutT>
class Receiver final {
	static_assert(wire::ByteStorage<StorageT>,
		"Receiver storage must satisfy the wire::ByteStorage contract");

public:
	using Storage = StorageT;
	using Block = cobs::RxBlock<StorageT>;
	using Packet = cobs::Packet<StorageT>;
	using Layout = LayoutT;

	// Storage hands back bytes, not objects, so nothing may need tearing down
	// before those bytes go back. The header is ints and pointers; keep it so.
	static_assert(std::is_trivially_destructible_v<Block>,
		"RxBlock must stay trivially destructible: storage releases raw bytes");

	/*
	 * The largest BODY this instance accepts, from Format (§9.2). Not the
	 * largest decoded frame — that is length_size bytes more, which is why
	 * this is no longer called max_decoded_size: that name was one header away
	 * from the truth, on a layer where being one header out is the easiest
	 * mistake there is.
	 */
	static constexpr std::size_t max_receive_size = Layout::max_receive_size;
	static_assert(max_receive_size <= UINT16_MAX, "RxBlock::size is a uint16_t");

	static constexpr std::size_t length_size = Layout::length_size;

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
	template<::crc::Policy CrcT>
	void consume(CrcT& crc, std::span<const uint8_t> bytes) noexcept
	{
		static_assert(CrcT::wire_size == Layout::crc_size);
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
				onFrameComplete(crc, r.decoded_size);
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
	 *     Header  ->  parse N  ->  acquire_rx(header + N)  ->  Body  ->  publish
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
	 * The only place a packet is allocated, and the only place a block is
	 * constructed and `owner` set.
	 *
	 * Storage hands out BYTES — sizeof(Block) plus the payload — and knows
	 * nothing about what goes in them. The RX vertical constructs the header
	 * in place and establishes ownership, which is where both belong: storage
	 * supplies memory and takes it back, and nothing else. (An earlier
	 * contract had storage stamp `owner`; a storage written to its letter then
	 * handed back a packet whose owner was null, and the first Packet release
	 * dereferenced it.)
	 */
	[[nodiscard]] Block* acquire_block(const std::size_t size) noexcept
	{
		std::byte* const memory = m_storage.acquire_rx(sizeof(Block) + size);
		if (memory == nullptr) {
			return nullptr;
		}
		Block* const block = std::construct_at(
			static_cast<Block*>(static_cast<void*>(memory)));
		block->owner = &m_storage;
		return block;
	}

	[[nodiscard]] static std::byte* bytes_of(Block* const block) noexcept
	{
		return static_cast<std::byte*>(static_cast<void*>(block));
	}

	// Turns a complete header into an allocated packet, or refuses the frame.
	void beginBody() noexcept
	{
		const std::size_t declared = Layout::load_length(m_lengthBytes.data());

		if (declared > Layout::max_receive_body) {
			++m_stats.oversize;
			abandonFrame();
			return;
		}
		if (declared == 0u || declared < Layout::crc_size) {
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

	template<::crc::Policy CrcT>
	void onFrameComplete(CrcT& crc, const std::size_t decoded_size) noexcept
	{
		if (m_stage != Stage::Body) {
			// The delimiter arrived before any body did. Only one of these is
			// a real frame.
			if (decoded_size < length_size) {
				++m_stats.length_mismatch; // header absent or truncated
				endFrame(true);
				return;
			}
			const std::size_t declared = Layout::load_length(m_lengthBytes.data());
			// Oversize is decided by the HEADER, so it has to be decided the
			// same way whether or not a body ever started. Without this the
			// classification would depend on the frame's punctuation: a frame
			// declaring 65 with one body byte is oversize, and the same frame
			// with no body at all would be a mere length mismatch.
			if (declared > Layout::max_receive_body) {
				++m_stats.oversize;
				endFrame(true); // the delimiter is already consumed: no resync
				return;
			}
			if (declared != 0u || Layout::crc_size != 0u) {
				++m_stats.length_mismatch; // declared a body, sent none
				endFrame(true);
				return;
			}
			// Only a zero-width policy can have an empty body.
			if (!::crc::verify(std::span<const uint8_t>{}, crc)) {
				++m_stats.crc_errors;
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
		if (!::crc::verify(m_building->data(), crc)) {
			++m_stats.crc_errors;
			endFrame(true); // delimiter consumed: next frame must not be lost
			return;
		}
		publish(body - Layout::crc_size);
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
			m_storage.release_rx(bytes_of(m_building));
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

	std::array<uint8_t, Layout::length_size> m_lengthBytes{};
	Stage m_stage = Stage::NeedHeader;

	Block* m_building  = nullptr;
	Block* m_readyHead = nullptr;
	Block* m_readyTail = nullptr;
	cobs::Stats::Rx m_stats{};
};

} // namespace cobs::detail

#endif /* COBS_DETAIL_RECEIVER_H_ */
