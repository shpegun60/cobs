/*
 * CobsDecoder — the COBS framing state machine, and nothing else.
 *
 * Contract: doc/COBS_ENGINE.md. This class deliberately knows no allocator,
 * no transport and no ownership: it decodes into a span the owner supplies,
 * so it can be tested with no HAL, no pool and no transport at all, and it is
 * compiled exactly once no matter how many Cobs instantiations exist.
 *
 * Because it holds no allocator, an allocation failure cannot arise inside
 * it. The seam is Event::NeedOutput: the decoder consumes the code byte that
 * starts a frame, then asks its owner for somewhere to put the result.
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
 *             if (auto* p = pool.allocate()) { decoder.attach_output(p->payload()); }
 *             else                           { decoder.discard_until_delimiter(); }
 *             break;
 *         case Event::FrameComplete: publish(r.decoded_size); break;
 *         case Event::Malformed:
 *         case Event::Oversize:      release(); break;
 *         }
 *     }
 *
 * The decoder accepts any structurally valid COBS, including non-canonical
 * encodings such as a redundant trailing 01 block. Only the encoder is
 * required to be canonical.
 */

#ifndef COBS_DECODER_H_
#define COBS_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <span>

class CobsDecoder final {
public:
	enum class Event : uint8_t {
		None,          // the input ran out; nothing for the owner to do
		FrameComplete, // decoded_size bytes are in the attached output
		NeedOutput,    // a frame started; attach_output() or discard
		Malformed,     // delimiter inside a block: frame dropped, resynchronized
		Oversize,      // the frame does not fit the attached output
	};

	struct Result {
		std::size_t consumed = 0;      // bytes of the input taken
		std::size_t decoded_size = 0;  // meaningful on FrameComplete only
		Event event = Event::None;
	};

	// Decodes until the first event the owner must act on, or until the input
	// is exhausted. Never consumes past that event, so the owner can hand the
	// remainder straight back after reacting.
	//
	// consumed may be 0 on Oversize (see §5 of the contract: the implicit zero
	// can overflow before the code byte that triggered it is taken). That does
	// not livelock: the state has already changed to discarding, so the next
	// call makes progress.
	[[nodiscard]] Result consume(std::span<const uint8_t> input) noexcept;

	// Answer to NeedOutput. A zero-length span is legal — the capacity and the
	// "is anything attached" question are deliberately separate, so that a
	// protocol with MaxDecodedSize == 0 cannot be confused with an unanswered
	// NeedOutput.
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
		m_blockRemaining = 0;
		m_pendingZero = false;
	}

	State m_state = State::Synced;

	std::span<uint8_t> m_output{};
	std::size_t m_written = 0;

	uint8_t m_blockRemaining = 0;
	bool m_pendingZero = false;
	bool m_hasOutput = false; // NOT m_output.empty(): see attach_output()
};

#endif /* COBS_DECODER_H_ */
