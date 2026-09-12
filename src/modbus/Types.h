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
// default RTU CRC16 and TCP NoCrc formats preserve them. Explicit Format size
// arguments count function-data bytes; each protocol adds its envelope and
// selected CRC width automatically without reducing the requested data limit.
// A standard PDU is one function byte plus zero to 252 data bytes.
inline constexpr std::size_t max_pdu_size = 253u;
inline constexpr std::size_t max_data_size = max_pdu_size - 1u;

// Protocol metadata changes the frame, not the transport/ownership result.
using wire::SendResult;

} // namespace modbus

#endif /* MODBUS_TYPES_H_ */
