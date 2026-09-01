/*
 * cobs::codec — the non-template COBS framing primitives.
 *
 * Contract: doc/PROTOCOL.md. The detailed derivation and overlap proof remain
 * in doc/COBS_ENGINE.md. This class deliberately knows no allocator, no
 * transport and no ownership: it decodes into a span the owner supplies, so
 * it can be tested with no HAL, no pool and no transport at all, and it is
 * compiled exactly once no matter how many Endpoint instantiations exist.
 *
 * Because it holds no allocator, an allocation failure cannot arise inside
 * it. The seam is Event::NeedOutput: the decoder consumes the code byte that
 * starts a frame, then asks its owner for somewhere to put the result.
 *
 * OUTPUT IS SEGMENTED. NeedOutput may be raised MANY times within one frame:
 * once when the frame starts, and again whenever the attached segment is full
 * and another decoded byte is due. The owner answers each one with the next
 * segment, or discards. This is what lets a length-prefixed protocol decode a
 * small header into a local buffer, learn how big the body is, allocate for
 * exactly that, and continue into the packet — with no staging buffer and no
 * copy. The decoder itself knows nothing about any of that; it only knows it
 * has run out of room.
 *
 * decoded_size on FrameComplete is the TOTAL across every segment.
 *
 * A NeedOutput raised for a full segment does NOT consume the encoded byte
 * that still needs a home, so no input is lost when the owner attaches more.
 *
 * Usage:
 *
 *     for (;;) {
 *         const auto r = decoder.consume(input);
 *         input = input.subspan(r.consumed);
 *         switch (r.event) {
 *         case Event::None:          // input exhausted
 *             return;
 *         case Event::NeedOutput:
 *             if (auto seg = owner.next_segment(); !seg.empty()) {
 *                 decoder.attach_output(seg);
 *             } else {
 *                 decoder.discard_until_delimiter();  // no more room allowed
 *             }
 *             break;
 *         case Event::FrameComplete: publish(r.decoded_size); break;
 *         case Event::Malformed:     release(); break;
 *         }
 *     }
 *
 * An owner that answers NeedOutput with a zero-length segment gets asked
 * again, forever. "No more output" is expressed by discarding, not by
 * attaching nothing.
 *
 * The decoder accepts any structurally valid COBS, including non-canonical
 * encodings such as a redundant trailing 01 block. Only the encoder is
 * required to be canonical.
 */

#ifndef COBS_CODEC_H_
#define COBS_CODEC_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace cobs::codec {

class Decoder final {
public:
	enum class Event : uint8_t {
		None,          // the input ran out; nothing for the owner to do
		FrameComplete, // decoded_size bytes were written, across all segments
		NeedOutput,    // room is needed: attach the next segment, or discard
		Malformed,     // delimiter inside a block: frame dropped, resynchronized
	};
	// There is deliberately no Oversize. "Too big" is not a fact this class
	// can know: it has no protocol limit, only a segment that happens to be
	// full, and a full segment is now a request rather than a verdict. The
	// layer that knows rx_max_size and the declared length decides that.

	struct Result {
		std::size_t consumed = 0;      // bytes of the input taken
		std::size_t decoded_size = 0;  // meaningful on FrameComplete only
		Event event = Event::None;
	};

	// Decodes until the first event the owner must act on, or until the input
	// is exhausted. Never consumes past that event, so the owner can hand the
	// remainder straight back after reacting.
	//
	// consumed may be 0 on NeedOutput — deliberately: the byte that needs room
	// is left in the input so it lands in the next segment. That cannot
	// livelock, because the owner must either attach room or discard, and both
	// change the state.
	[[nodiscard]] Result consume(std::span<const uint8_t> input) noexcept;

	// Answer to NeedOutput: the NEXT output segment. Accepted only when one is
	// actually wanted — while decoding, with nothing attached yet or with the
	// current segment full. Attaching over a segment that still has room would
	// silently strand the bytes already in it, so it is ignored.
	//
	// Only the segment-local write position resets; the running total does
	// not, so decoded_size still spans the whole frame.
	void attach_output(std::span<uint8_t> output) noexcept;

	// The only reset there is: forget the frame being built and discard bytes
	// until the next delimiter. Used for an allocation failure and for a
	// transport gap.
	//
	// There is deliberately no reset()-to-Synced: after a physical loss there
	// is no evidence the next byte begins a frame, and a public button that
	// skipped straight to Synced would be a button for breaking the framing
	// contract quickly. Tests that want a pristine decoder construct one.
	void discard_until_delimiter() noexcept;

private:
	enum class State : uint8_t { Synced, Decoding, DropUntilDelimiter };

	void beginBlock(uint8_t code) noexcept
	{
		m_blockRemaining = static_cast<uint8_t>(code - 1u);
		m_pendingZero = (code != 0xFFu); // 0xFF carries no implicit zero
	}

	void dropFrame() noexcept
	{
		m_output = {};
		m_hasOutput = false;
		m_written = 0;
		m_decodedBefore = 0;
		m_blockRemaining = 0;
		m_pendingZero = false;
	}

	// True when the next decoded byte has nowhere to go. Only asked once a
	// segment has actually been attached.
	[[nodiscard]] bool segmentFull() const noexcept
	{
		return m_written == m_output.size();
	}

	[[nodiscard]] std::size_t decodedTotal() const noexcept
	{
		return m_decodedBefore + m_written;
	}

	State m_state = State::Synced;

	std::span<uint8_t> m_output{};
	std::size_t m_written = 0;       // bytes written into the CURRENT segment
	std::size_t m_decodedBefore = 0; // bytes written into all previous ones

	uint8_t m_blockRemaining = 0;
	bool m_pendingZero = false;
	bool m_hasOutput = false; // NOT m_output.empty(): see attach_output()
};

/*
 * Canonical in-place encoding and its storage geometry.
 *
 * The raw decoded bytes begin at raw_offset(N) inside the caller's block. The
 * encoder writes forward from block[0] while reading ahead from that offset.
 * The extra headroom preserves the reviewed overlap invariant:
 *
 *     written <= consumed + raw_offset(N) - 1
 *
 * max_encoded_size() excludes the delimiter; max_wire_size() and the span
 * returned from encode_in_place() include it.
 */

[[nodiscard]] constexpr std::size_t max_encoded_size(const std::size_t size) noexcept
{
	return (size == 0u) ? 1u : size + (size + 253u) / 254u;
}

[[nodiscard]] constexpr std::size_t max_wire_size(const std::size_t size) noexcept
{
	return max_encoded_size(size) + 1u;
}

[[nodiscard]] constexpr std::size_t raw_offset(const std::size_t max_decoded) noexcept
{
	return max_wire_size(max_decoded) - max_decoded;
}

[[nodiscard]] constexpr bool size_arithmetic_fits(const std::size_t size) noexcept
{
	// Compute overhead separately so the guard itself cannot wrap while
	// deciding whether size + overhead is representable.
	if (size == 0u) {
		return true;
	}
	const std::size_t overhead =
		size / 254u + ((size % 254u != 0u) ? 1u : 0u) + 1u;
	return size <= static_cast<std::size_t>(-1) - overhead;
}

// Returns an empty span when the raw range is outside storage, storage is too
// small for the worst case, or offset violates the overlap invariant. A valid
// result is canonical: no redundant 01 follows a final full FF block, while a
// payload ending in zero retains the final 01 that materializes that zero.
[[nodiscard]] std::span<const uint8_t> encode_in_place(
	std::span<uint8_t> storage,
	std::size_t offset,
	std::size_t raw_size) noexcept;

} // namespace cobs::codec

#endif /* COBS_CODEC_H_ */
