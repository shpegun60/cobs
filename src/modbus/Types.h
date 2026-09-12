/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef MODBUS_TYPES_H_
#define MODBUS_TYPES_H_

#include "../wire/SendResult.h"

#include <cstddef>
#include <cstdint>

namespace modbus {

// Standard Modbus application-protocol limits shared by RTU and TCP. The
// default RTU CRC16 format preserves them. An explicitly nonstandard RTU CRC
// policy may expose a different effective limit inside the default 256-byte
// ADU; an explicit Format can also select a different physical ADU ceiling.
// A standard PDU is one function byte plus zero to 252 data bytes.
inline constexpr std::size_t max_pdu_size = 253u;
inline constexpr std::size_t max_data_size = max_pdu_size - 1u;

// Protocol metadata changes the frame, not the transport/ownership result.
using wire::SendResult;

} // namespace modbus

#endif /* MODBUS_TYPES_H_ */
