/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * An independent oracle for the ENGINE wire format, for tests only.
 *
 * doc/PROTOCOL.md: an engine frame is a COBS-encoded [length][body],
 * where the length is a little-endian field of 1 or 2 bytes counting only the
 * body that follows it.
 *
 *     wire = reference_cobs_encode( length_prefix + payload )
 *
 * Kept deliberately separate from reference_encoder.h. That one is the oracle
 * for PURE COBS and must stay ignorant of this protocol; the decoder and
 * encoder suites check byte strings that have nothing to do with an engine.
 * This one composes the two, by hand, so a mistake in cobs::Format cannot
 * hide inside the thing that is supposed to catch it.
 */
#ifndef COBS_TEST_REFERENCE_FRAME_H_
#define COBS_TEST_REFERENCE_FRAME_H_

#include "reference_encoder.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cobs_test {

// The decoded frame: the little-endian length field, then the body.
inline std::vector<uint8_t> decoded_frame(const std::vector<uint8_t>& payload,
                                          const std::size_t length_size)
{
	assert(length_size == 1u || length_size == 2u);
	std::vector<uint8_t> out;
	out.push_back(static_cast<uint8_t>(payload.size() & 0xFFu));
	if (length_size == 2u) {
		out.push_back(static_cast<uint8_t>((payload.size() >> 8) & 0xFFu));
	}
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

// The complete wire frame, delimiter included.
inline std::vector<uint8_t> frame(const std::vector<uint8_t>& payload,
                                  const std::size_t length_size)
{
	return encode(decoded_frame(payload, length_size));
}

// A frame whose declared length deliberately disagrees with its body, for the
// malformed-length battery.
inline std::vector<uint8_t> frame_declaring(const std::size_t declared,
                                            const std::vector<uint8_t>& body,
                                            const std::size_t length_size)
{
	std::vector<uint8_t> decoded;
	decoded.push_back(static_cast<uint8_t>(declared & 0xFFu));
	if (length_size == 2u) {
		decoded.push_back(static_cast<uint8_t>((declared >> 8) & 0xFFu));
	}
	decoded.insert(decoded.end(), body.begin(), body.end());
	return encode(decoded);
}

// A frame whose decoded content is exactly `decoded` — used to build headers
// that are truncated or absent altogether.
inline std::vector<uint8_t> frame_of_decoded(const std::vector<uint8_t>& decoded)
{
	return encode(decoded);
}

} // namespace cobs_test

#endif /* COBS_TEST_REFERENCE_FRAME_H_ */
