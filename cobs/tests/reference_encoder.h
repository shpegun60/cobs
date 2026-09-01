/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * A canonical/minimal COBS encoder for TESTS ONLY.
 *
 * Straightforward and allocating, precisely because the production in-place
 * encoder (doc/COBS_ENGINE.md §8.3) will be neither: an oracle that shares
 * the tricks of the thing it checks is not an oracle.
 */
#ifndef COBS_TEST_REFERENCE_ENCODER_H_
#define COBS_TEST_REFERENCE_ENCODER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cobs_test {

// Encodes payload and appends the frame delimiter.
inline std::vector<uint8_t> encode(const std::vector<uint8_t>& payload)
{
	std::vector<uint8_t> out;
	std::size_t code_pos = 0;
	uint8_t code = 1;

	out.push_back(0); // placeholder for the first code
	for (const uint8_t b : payload) {
		if (b != 0u) {
			out.push_back(b);
			++code;
			if (code != 0xFFu) {
				continue;
			}
		}
		out[code_pos] = code;
		code_pos = out.size();
		out.push_back(0); // placeholder for the next code
		code = 1;
	}
	out[code_pos] = code;

	// A trailing 0xFF block owes no zero, so the canonical encoding stops
	// there rather than emitting an empty 01 block after it.
	if (code_pos == out.size() - 1u && out[code_pos] == 1u && code_pos >= 255u &&
	    out[code_pos - 255u] == 0xFFu && !payload.empty() && payload.back() != 0u) {
		out.pop_back();
	}
	out.push_back(0); // delimiter
	return out;
}

} // namespace cobs_test

#endif /* COBS_TEST_REFERENCE_ENCODER_H_ */
