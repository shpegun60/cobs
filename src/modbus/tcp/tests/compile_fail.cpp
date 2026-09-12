/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "modbus/tcp/Tcp.h"
#if TCP_BAD == 1
modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<crc::NoCrc, SIZE_MAX>> bad;
#elif TCP_BAD == 2
modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<crc::NoCrc, 65534u>> bad;
#elif TCP_BAD == 3
modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<crc::Crc64Table, 65526u>> bad;
#elif TCP_BAD == 4
struct BadMemory {};
modbus::tcp::Endpoint<BadMemory> bad;
#elif TCP_BAD == 5
struct BadCrc {};
modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<BadCrc>> bad;
#elif TCP_BAD == 6
void bad() { modbus::tcp::Endpoint<> e; auto m = e.make_message(0, 0, 0); (void)m.append_be(true); }
#elif TCP_BAD == 7
void bad() { modbus::tcp::Endpoint<> e; auto m = e.make_message(0, 0, 0); crc::NoCrc c; (void)m.finalize(c); }
#elif TCP_BAD == 8
modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<>, int> bad; // MBAP, no framer parameter
#elif TCP_BAD == 9
static_assert(sizeof(modbus::tcp::Layout<SIZE_MAX, 0u>) != 0u);
#endif
