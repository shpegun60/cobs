/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Application data plus all automatically added bytes must fit the ADU size
 * metadata. Reject one byte over the CRC16 limit before any allocation.
 */
#include "modbus/rtu/Rtu.h"

modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<::crc::Crc16Bitwise, 65532u>> endpoint;
