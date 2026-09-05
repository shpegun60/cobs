/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Physical Modbus RTU limits shared by every wire policy and storage. */

#ifndef MODBUS_RTU_LIMITS_H_
#define MODBUS_RTU_LIMITS_H_

#include <cstddef>

namespace modbus::rtu {

inline constexpr std::size_t max_adu_size = 256u;

} // namespace modbus::rtu

#endif /* MODBUS_RTU_LIMITS_H_ */
