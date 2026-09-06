/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

// Must not compile: the default (burst) RTU endpoint expects one complete ADU
// per delivery, and a QSerialPort delivers arbitrary cuts.
// Expected diagnostic: "an RTU endpoint over QSerialPort needs a framing policy"

#include "adapters/qt/SerialAdapter.h"
#include "modbus/rtu/Rtu.h"

using Burst = modbus::rtu::Endpoint<wire::Heap>;

int main()
{
	QSerialPort port;
	Burst link;
	adapters::qt::SerialAdapter<Burst> adapter{port, link};
	return adapter.bind() ? 0 : 1;
}
