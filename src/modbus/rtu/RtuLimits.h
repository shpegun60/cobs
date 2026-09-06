/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * The physical ceiling the Modbus serial line specification puts on one RTU
 * ADU: 256 bytes, address and CRC included. It is the DEFAULT of
 * modbus::rtu::Format's MaxAdu parameter, not a hard-wired limit: a device
 * that only ever handles small PDUs may bind a smaller ceiling to save RAM
 * (a local capacity choice, still standard Modbus on the wire), and a private
 * RTU-like protocol on a fast link may bind a larger one (no longer standard
 * Modbus RTU; both peers must agree, and the UART adapter must deliver the
 * whole candidate in one burst).
 */

#ifndef MODBUS_RTU_LIMITS_H_
#define MODBUS_RTU_LIMITS_H_

#include <cstddef>

namespace modbus::rtu {

inline constexpr std::size_t standard_adu_size = 256u;

} // namespace modbus::rtu

#endif /* MODBUS_RTU_LIMITS_H_ */
