/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

// Compile-only integration probe against the real F1/G4/H7RS HAL headers.
// Runtime fault recovery is exercised by test_uart_integration.cpp, not here.
#include "Uart.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/rtu/UartAdapter.h"
#include "cobs/Cobs.h"
#include "adapters/cobs/UartAdapter.h"

namespace {
using Serial = Uart<256, 4>;
using Link = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
	modbus::rtu::framing::Standard<modbus::rtu::framing::Direction::Request>>;
using Adapter = modbus::rtu::UartAdapter<Serial, Link>;
using CobsLink = cobs::Endpoint<wire::Pool<4, 2>>;
using CobsAdapter = cobs::UartAdapter<Serial, CobsLink>;

static_assert(sizeof(Adapter) == 32u, "ARM adapter state, including the overdue progress baseline");
static_assert(sizeof(CobsAdapter) == 12u, "COBS adapter: two references and bound flag, no timer state");
static_assert(sizeof(Serial) == 1696u, "slow-path recovery adds no UART state");
static_assert(Adapter::chunk_time_ms(9600u) == 320u);
static_assert(Adapter::chunk_time_ms(115200u) == 27u);

Serial serial;
Link link;
Adapter adapter{serial, link};
CobsLink cobs_link;
CobsAdapter cobs_adapter{serial, cobs_link}; // compile-only; never bind both adapters at once
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
	adapter.proceed();
	(void)adapter.deadline_in_ms();
	return adapter.unbind();
}

extern "C" bool cobs_uart_adapter_port_test(UART_HandleTypeDef* handle,
                                           const uint32_t now,
                                           const std::span<const uint8_t> bytes)
{
	if (!serial.init(handle) || !cobs_adapter.bind()) {
		return false;
	}
	cobs_adapter.on_rx(bytes);
	cobs_adapter.on_gap();
	(void)cobs_adapter.deadline_in_ms(now);
	cobs_adapter.proceed(now);
	cobs_adapter.proceed();
	(void)cobs_adapter.deadline_in_ms();
	return cobs_adapter.unbind();
}
