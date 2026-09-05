/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "Cobs.h"
cobs::Endpoint<wire::Heap, cobs::Format<crc::Crc16Bitwise, 65534u>> endpoint;
