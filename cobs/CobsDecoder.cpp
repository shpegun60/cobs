#include "CobsDecoder.h"

void CobsDecoder::attach_output(const std::span<uint8_t> output) noexcept
{
	// Only meaningful as the answer to NeedOutput. While discarding there is
	// no frame to decode into, and accepting a buffer would strand it.
	if (m_state != State::Decoding || m_hasOutput) {
		return;
	}
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
			// The owner has not answered NeedOutput yet. Ask again rather than
			// write anywhere; it must attach or discard before progress.
			return {i, 0, Event::NeedOutput};
		}

		if (m_blockRemaining > 0u) {
			// Data position. A zero cannot legally appear here: the encoder
			// never produces one, so this is corruption or truncation. Checked
			// before capacity — it is a framing error regardless of space.
			if (b == 0u) {
				++i;
				dropFrame();
				m_state = State::Synced; // this delimiter already resynchronized us
				return {i, 0, Event::Malformed};
			}
			if (m_written == m_output.size()) {
				++i; // the byte belongs to a frame that is already doomed
				dropFrame();
				m_state = State::DropUntilDelimiter;
				return {i, 0, Event::Oversize};
			}
			m_output[m_written] = b;
			++m_written;
			--m_blockRemaining;
			++i;
			continue;
		}

		// Code position.
		if (b == 0u) {
			++i;
			const std::size_t size = m_written; // pendingZero is NOT materialized
			dropFrame();
			m_state = State::Synced;
			return {i, size, Event::FrameComplete};
		}

		// Another block follows, so the zero owed by the previous one is real.
		if (m_pendingZero) {
			if (m_written == m_output.size()) {
				// Overflow detected BEFORE this code byte is taken, so consumed
				// may be 0. Safe: the state below changes, so the next call
				// consumes in DropUntilDelimiter.
				dropFrame();
				m_state = State::DropUntilDelimiter;
				return {i, 0, Event::Oversize};
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
