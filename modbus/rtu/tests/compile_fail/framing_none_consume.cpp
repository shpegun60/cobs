/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Without a framing policy there is no stream assembly: consume() must not
 * exist on the default endpoint, so a caller cannot feed it arbitrary chunks
 * believing the frame ends will be found.
 */
#include "modbus/rtu/Rtu.h"

#include <cstdint>
#include <span>

void feed(modbus::rtu::Endpoint<>& endpoint, std::span<const uint8_t> bytes)
{
	endpoint.consume(bytes);
}
