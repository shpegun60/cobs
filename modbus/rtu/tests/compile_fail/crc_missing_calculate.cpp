/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"

struct MissingCalculate final {};

modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<MissingCalculate>> endpoint;
