/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"

struct MissingCalculate final {};

modbus::rtu::Endpoint<modbus::rtu::Heap, MissingCalculate> endpoint;
