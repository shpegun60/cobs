/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * An ADU ceiling below address + function must be rejected BEFORE the layout
 * subtracts the prefix from it. With an unsigned ceiling of 1 the subtraction
 * wraps, the CRC-fits check passes by accident, max_data_size becomes
 * enormous, and make_message() writes a two-byte header into a one-byte
 * allocation (found by ASan).
 */
#include "modbus/rtu/Rtu.h"

modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<::crc::Crc16Bitwise, 1u>> endpoint;
