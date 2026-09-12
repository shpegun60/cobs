/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

// A framed RTU endpoint has consume(), but MUST NOT silently get the COBS
// adapter: that would omit the stale-frame supervision its stream requires.
#include "adapters/cobs/UartAdapter.h"
#include "modbus/rtu/Rtu.h"

struct Serial {};
using Endpoint = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>,
	modbus::rtu::framing::Standard<modbus::rtu::framing::Direction::Request>>;
Serial serial;
Endpoint endpoint;
cobs::UartAdapter<Serial, Endpoint> adapter{serial, endpoint};
