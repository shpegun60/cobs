#include "CobsDecoder.h"

void CobsDecoder::attach_output(const std::span<uint8_t> output) noexcept
{
	// Only meaningful as the answer to NeedOutput. While discarding there is
	// no frame to decode into, and accepting a buffer would strand it.
	if (m_state != State::Decoding) {
		return;
	}
	// A segment that still has room is not being asked for. Replacing it would
	// silently drop the bytes already written into it, which is exactly the
	// kind of loss this class exists to make impossible.
	if (m_hasOutput && m_written != m_output.size()) {
		return;
	}
	m_decodedBefore += m_written; // the finished segment joins the total
	m_output = output;
	m_written = 0;
	m_hasOutput = true;
}

void CobsDecoder::discard_until_delimiter() noexcept
{
	dropFrame();
	m_state = State::DropUntilDelimiter;
}

CobsDecoder::Result CobsDecoder::consume(const std::span<const uint8_t> input) noexcept
{
	std::size_t i = 0;

	while (i < input.size()) {
		const uint8_t b = input[i];

		if (m_state == State::DropUntilDelimiter) {
			++i;
			if (b == 0u) {
				m_state = State::Synced; // framing restored
			}
			continue;
		}

		if (m_state == State::Synced) {
			++i;
			if (b == 0u) {
				continue; // a bare delimiter is a no-op, never a packet
			}
			// A frame starts here. The code byte is consumed now so the owner
			// never has to feed the same byte twice, and a normal frame start
			// never reports consumed == 0.
			beginBlock(b);
			m_state = State::Decoding;
			return {i, 0, Event::NeedOutput};
		}

		// State::Decoding
		if (!m_hasOutput) {
			// The owner has not answered the FIRST NeedOutput yet. Ask again
			// rather than write anywhere — and in particular do not report a
			// completion, which would invent an empty frame out of an
			// unanswered question.
			return {i, 0, Event::NeedOutput};
		}

		if (m_blockRemaining > 0u) {
			// Data position. A zero cannot legally appear here: the encoder
			// never produces one, so this is corruption or truncation. Checked
			// before capacity — it is a framing error regardless of space, and
			// asking for a segment to put a malformed byte in would be absurd.
			if (b == 0u) {
				++i;
				dropFrame();
				m_state = State::Synced; // this delimiter already resynchronized us
				return {i, 0, Event::Malformed};
			}
			if (segmentFull()) {
				// NOT consumed: this byte belongs in the next segment. The
				// owner attaches one or discards, so the state changes either
				// way and the next call makes progress.
				return {i, 0, Event::NeedOutput};
			}
			m_output[m_written] = b;
			++m_written;
			--m_blockRemaining;
			++i;
			continue;
		}

		// Code position.
		if (b == 0u) {
			// A frame ends exactly here, whatever the state of the current
			// segment: a full one is fine, and no further room is needed. The
			// pending zero is NOT materialized — a block followed by the
			// delimiter owes nothing.
			++i;
			const std::size_t size = decodedTotal();
			dropFrame();
			m_state = State::Synced;
			return {i, size, Event::FrameComplete};
		}

		// Another block follows, so the zero owed by the previous one is real.
		if (m_pendingZero) {
			if (segmentFull()) {
				// The implicit zero needs room too, and the code byte that
				// revealed it is deliberately left unconsumed.
				return {i, 0, Event::NeedOutput};
			}
			m_output[m_written] = 0u;
			++m_written;
			m_pendingZero = false;
		}
		++i;
		beginBlock(b);
	}

	return {i, 0, Event::None};
}
