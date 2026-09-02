/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"

#include <cstdint>

struct WireRecord final {
	uint8_t type;
	uint32_t value;
};

int main()
{
	modbus::rtu::Endpoint<> endpoint;
	auto message = endpoint.make_message(1u, 3u);
	(void)message.append_be(WireRecord{1u, 2u});
}
