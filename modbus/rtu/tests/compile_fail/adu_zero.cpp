/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "modbus/rtu/Rtu.h"
modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<crc::NoCrc, 0>> endpoint;
