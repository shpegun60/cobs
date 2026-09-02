/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef MODBUS_TYPES_H_
#define MODBUS_TYPES_H_

#include <cstddef>
#include <cstdint>

namespace modbus {

// The application-protocol limit shared by RTU and TCP. A PDU is one
// function byte followed by zero to 252 function-data bytes.
inline constexpr std::size_t max_pdu_size = 253u;
inline constexpr std::size_t max_data_size = max_pdu_size - 1u;

// The transport handshake is intentionally identical to cobs::SendResult.
// Protocol-specific metadata changes the frame, not ownership semantics.
enum class SendResult : uint8_t {
	Sent,
	Busy,
	Unbound,
	Failed,
	Invalid,
};

} // namespace modbus

#endif /* MODBUS_TYPES_H_ */
