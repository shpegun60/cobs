/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "Codec.h"

namespace cobs::codec {

void Decoder::attach_output(const std::span<uint8_t> output) noexcept
{
	// Only meaningful as the answer to NeedOutput. While synchronized or
	// discarding there is no current segment to replace.
	if (m_state != State::Decoding) {
		return;
	}
	// A segment that still has room is not being asked for. Replacing it would
	// silently drop the bytes already written into it.
	if (m_hasOutput && m_written != m_output.size()) {
		return;
	}
	m_decodedBefore += m_written; // the finished segment joins the total
	m_output = output;
	m_written = 0;
	m_hasOutput = true;
}

void Decoder::discard_until_delimiter() noexcept
{
	dropFrame();
	m_state = State::DropUntilDelimiter;
}

Decoder::Result Decoder::consume(const std::span<const uint8_t> input) noexcept
{
	std::size_t i = 0;

	// Discarding and synchronization are cold states. Finish them before the
	// byte-hot Decoding loop so that loop does not reload and branch on
	// m_state for every payload byte.
	while (m_state == State::DropUntilDelimiter && i < input.size()) {
		if (input[i++] == 0u) {
			m_state = State::Synced;
		}
	}

	while (m_state == State::Synced && i < input.size()) {
		const uint8_t code = input[i++];
		if (code == 0u) {
			continue; // a bare delimiter is a no-op, never a packet
		}
		// A frame starts here. The code byte is consumed now so the owner
		// never has to feed the same byte twice, and a normal frame start
		// never reports consumed == 0.
		beginBlock(code);
		m_state = State::Decoding;
		if (!m_hasOutput) {
			return {i, 0, Event::NeedOutput};
		}
	}

	if (i == input.size()) {
		return {i, 0, Event::None};
	}

	// State::Decoding. The owner has not answered the FIRST NeedOutput yet.
	// Ask again rather than write anywhere — and in particular do not report a
	// completion, which would invent an empty frame out of an unanswered
	// question.
	if (!m_hasOutput) {
		return {i, 0, Event::NeedOutput};
	}

	// These four values are the per-byte working set. Keeping them in locals
	// lets Cortex-M hold them in registers instead of loading/storing Decoder
	// fields for every decoded byte. They are published back at every exit
	// where the frame remains alive.
	uint8_t* const output = m_output.data();
	const std::size_t capacity = m_output.size();
	std::size_t written = m_written;
	uint8_t block_remaining = m_blockRemaining;
	bool pending_zero = m_pendingZero;

	const auto save = [&]() noexcept {
		m_written = written;
		m_blockRemaining = block_remaining;
		m_pendingZero = pending_zero;
	};

	while (i < input.size()) {
		const uint8_t b = input[i];

		if (block_remaining > 0u) {
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
			if (written == capacity) {
				// NOT consumed: this byte belongs in the next segment. The
				// owner attaches one or discards, so the state changes either
				// way and the next call makes progress.
				save();
				return {i, 0, Event::NeedOutput};
			}
			output[written++] = b;
			--block_remaining;
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
			const std::size_t size = m_decodedBefore + written;
			dropFrame();
			m_state = State::Synced;
			return {i, size, Event::FrameComplete};
		}

		// Another block follows, so the zero owed by the previous one is real.
		if (pending_zero) {
			if (written == capacity) {
				// The implicit zero needs room too, and the code byte that
				// revealed it is deliberately left unconsumed.
				save();
				return {i, 0, Event::NeedOutput};
			}
			output[written++] = 0u;
			pending_zero = false;
		}
		++i;
		block_remaining = static_cast<uint8_t>(b - 1u);
		pending_zero = (b != 0xFFu);
	}

	save();
	return {i, 0, Event::None};
}

} // namespace cobs::codec
