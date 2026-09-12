/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

// Compile-only integration probe against the real F1/G4/H7RS HAL headers.
// Runtime fault recovery is exercised by test_uart_integration.cpp, not here.
#include "Uart.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/rtu/UartAdapter.h"

namespace {
using Serial = Uart<256, 4>;
using Link = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
	modbus::rtu::framing::Standard<modbus::rtu::framing::Direction::Request>>;
using Adapter = modbus::rtu::UartAdapter<Serial, Link>;

static_assert(sizeof(Adapter) == 32u, "ARM adapter state, including the overdue progress baseline");
static_assert(sizeof(Serial) == 1696u, "slow-path recovery adds no UART state");
static_assert(Adapter::chunk_time_ms(9600u) == 320u);
static_assert(Adapter::chunk_time_ms(115200u) == 27u);

Serial serial;
Link link;
Adapter adapter{serial, link};
} // namespace

extern "C" bool uart_adapter_port_test(UART_HandleTypeDef* handle,
                                      const uint32_t now,
                                      const std::span<const uint8_t> bytes)
{
	if (!serial.init(handle) || !adapter.bind()) {
		return false;
	}
	adapter.prepare(now);
	serial.proceed(now);
	adapter.finish(now);
	adapter.on_rx(bytes);
	adapter.on_gap();
	(void)adapter.deadline_in_ms(now);
	adapter.proceed(now);
	return adapter.unbind();
}
