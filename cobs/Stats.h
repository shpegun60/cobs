/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Public COBS counter snapshot.
 *
 * Counters stay physically beside the state machines that update them:
 * Receiver owns Rx and Endpoint owns Tx. Endpoint::stats() combines copies of
 * both into this value type, so an application observes one coherent
 * API without receiving mutable references into engine state.
 *
 * Event counters are uint32_t and intentionally wrap modulo 2^32. Keeping an
 * update to one native increment matters on embedded hot paths; monitoring
 * that needs lifetime totals should sample periodically and extend modular
 * deltas in its own wider accumulator.
 */

#ifndef COBS_STATS_H_
#define COBS_STATS_H_

#include <cstdint>

namespace cobs {

struct Stats final {
	struct Rx final {
		uint32_t frames_delivered   = 0;
		uint32_t frames_lost        = 0; // every frame that did not reach the queue
		uint32_t allocation_failure = 0;
		uint32_t malformed          = 0; // structural COBS error
		uint32_t oversize           = 0; // declared length above max_receive_size
		uint32_t length_mismatch    = 0; // absent/short header, or body != declared
		uint32_t resyncs            = 0; // times RX had to hunt for a delimiter
	};

	struct Tx final {
		uint32_t frames_sent       = 0;
		uint32_t send_refused_busy = 0;
		uint32_t send_failed       = 0;
	};

	Rx rx{};
	Tx tx{};
};

} // namespace cobs

#endif /* COBS_STATS_H_ */
