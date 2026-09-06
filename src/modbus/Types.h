/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef MODBUS_TYPES_H_
#define MODBUS_TYPES_H_

#include <cstddef>
#include <cstdint>

namespace modbus {

// Standard Modbus application-protocol limits shared by RTU and TCP. The
// default RTU CRC16 format preserves them. An explicitly nonstandard RTU CRC
// policy may expose a different effective limit inside its fixed 256-byte ADU.
// A standard PDU is one function byte plus zero to 252 data bytes.
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
