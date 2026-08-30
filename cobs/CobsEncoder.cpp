#include "CobsEncoder.h"

std::span<const uint8_t> cobs_encode_in_place(
	const std::span<uint8_t> storage,
	const std::size_t raw_offset,
	const std::size_t raw_size) noexcept
{
	// The payload must actually be inside the storage...
	if (raw_offset > storage.size() || raw_size > storage.size() - raw_offset) {
		return {};
	}
	// ...the storage must be able to hold the worst-case frame...
	if (storage.size() < cobs_max_wire_size(raw_size)) {
		return {};
	}
	// ...and the headroom must satisfy the overlap invariant of §8.4. Checked
	// rather than assumed: this is the one precondition whose violation would
	// corrupt the payload silently instead of failing.
	if (raw_offset < cobs_raw_offset(raw_size)) {
		return {};
	}

	std::size_t w = 0;              // next byte to write
	std::size_t code_pos = w++;     // reserved slot for the current block's code
	uint8_t code = 1;               // bytes in this block, including the code
	bool last_block_was_ff = false;

	for (std::size_t i = 0; i < raw_size; ++i) {
		const uint8_t b = storage[raw_offset + i];
		if (b != 0u) {
			storage[w] = b;
			++w;
			++code;
			if (code != 0xFFu) {
				continue;
			}
			// 254 data bytes: this block is full and, being 0xFF, owes no
			// implicit zero.
		}
		// Either a zero terminated the run (the code byte stands in for it)
		// or the block filled up. Close it and reserve the next code slot.
		storage[code_pos] = code;
		last_block_was_ff = (code == 0xFFu);
		code_pos = w++;
		code = 1;
	}

	// Canonical ending. An empty final block is REQUIRED when the payload
	// ended with a zero — that block is what materializes it on decode — and
	// REDUNDANT when the payload ended exactly on a full 0xFF block, which
	// owes nothing. The two look identical from the code value alone, hence
	// last_block_was_ff.
	if (code == 1u && last_block_was_ff) {
		w = code_pos; // give the reserved slot back
	} else {
		storage[code_pos] = code;
	}

	storage[w] = 0u; // delimiter
	++w;
	return storage.first(w);
}
