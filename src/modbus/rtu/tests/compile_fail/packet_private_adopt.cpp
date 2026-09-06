/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"

int main()
{
	using Packet = modbus::rtu::Endpoint<>::Packet;
	(void)Packet::adopt(nullptr);
}
