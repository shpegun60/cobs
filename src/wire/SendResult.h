/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef WIRE_SEND_RESULT_H_
#define WIRE_SEND_RESULT_H_

#include <cstdint>

namespace wire {

// Shared transport/ownership result, not confirmation of delivery to a peer.
// Protocol namespaces export this same type; there is no conversion layer.
enum class SendResult : uint8_t {
	Sent,     // accepted: the endpoint owns the block; the message is empty
	Busy,     // transport unavailable: the message retains its ownership/state
	Unbound,  // no sender / busy-query delegate pair is bound
	Failed,   // start refused: prepared message retained, retryable but not writable
	Invalid,  // empty/foreign message or invalid protocol-specific TX layout
};

} // namespace wire

#endif /* WIRE_SEND_RESULT_H_ */
