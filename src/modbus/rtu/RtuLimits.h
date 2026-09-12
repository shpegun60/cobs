/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * The physical ceiling the Modbus serial line specification puts on one RTU
 * ADU: 256 bytes, address and CRC included. This is a protocol constant, not
 * an application-supplied size. Format<Crc, MaxData> defaults to 252 useful
 * data bytes; the default CRC16 adds four envelope bytes for a 256-byte ADU.
 * A device that only ever handles small PDUs may bind a smaller data limit to save RAM
 * (a local capacity choice, still standard Modbus on the wire), and a private
 * RTU-like protocol on a fast link may bind a larger one (no longer standard
 * Modbus RTU; both peers must agree). A burst endpoint still needs one whole
 * candidate per receive_adu(); an endpoint with a framing policy can instead
 * assemble a large ADU from multiple UART chunks through consume().
 */

#ifndef MODBUS_RTU_LIMITS_H_
#define MODBUS_RTU_LIMITS_H_

#include <cstddef>

namespace modbus::rtu {

inline constexpr std::size_t standard_adu_size = 256u;

} // namespace modbus::rtu

#endif /* MODBUS_RTU_LIMITS_H_ */
