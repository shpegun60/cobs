/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"

int main()
{
	modbus::rtu::Endpoint<> endpoint;
	auto message = endpoint.make_message(1u, 3u);
	(void)message.finalize();
}
