/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef MODBUS_RTU_STATS_H_
#define MODBUS_RTU_STATS_H_

#include <cstdint>

namespace modbus::rtu {

struct Stats final {
	struct Rx final {
		uint32_t candidates = 0;
		uint32_t frames_received = 0;
		// Wire value mismatches the Endpoint's selected calculator.
		uint32_t crc_errors = 0;
		uint32_t too_short = 0;
		uint32_t oversize = 0;
		uint32_t allocation_failure = 0;
		uint32_t stream_gaps = 0;
	};

	struct Tx final {
		uint32_t frames_sent = 0;
		uint32_t send_refused_busy = 0;
		uint32_t send_failed = 0;
	};

	Rx rx{};
	Tx tx{};
};

/*
 * Counters that exist only on an Endpoint with a framing policy
 * (Framing.h). They live beside, not inside, Stats: the default endpoint's
 * Stats and its layout are untouched by the optional framer.
 */
struct FramingStats final {
	// RX function code the policy has no layout for.
	uint32_t unsupported_function = 0;
	// receive_adu(): a complete candidate whose length disagrees with its
	// function's layout.
	uint32_t length_mismatch = 0;
	// A declared frame skipped byte-exactly for lack of RX memory; the
	// stream stayed in step (allocation_failure counts it too).
	uint32_t skipped_frames = 0;
	// The remainder of a chunk dropped after an error, because the next frame
	// start is unknown until the next chunk.
	uint32_t resyncs = 0;
	// send(): message data disagrees with its function's layout.
	uint32_t tx_layout_rejected = 0;
};

} // namespace modbus::rtu

#endif /* MODBUS_RTU_STATS_H_ */
