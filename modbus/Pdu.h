/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Public Modbus names for the transport-neutral, stateless payload readers.
 * Packet owns no cursor; the application owns `offset`. These using-declarations
 * add no wrapper and therefore no protocol-specific runtime layer.
 */

#ifndef MODBUS_PDU_H_
#define MODBUS_PDU_H_

#include "Types.h"
#include "../wire/Read.h"

namespace modbus {

using wire::read_native;
using wire::read_be;
using wire::read_le;
using wire::read_bytes;

} // namespace modbus

#endif /* MODBUS_PDU_H_ */
