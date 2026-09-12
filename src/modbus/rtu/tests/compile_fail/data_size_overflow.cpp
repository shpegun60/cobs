/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "modbus/rtu/Rtu.h"
modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<crc::NoCrc, SIZE_MAX>> endpoint;
