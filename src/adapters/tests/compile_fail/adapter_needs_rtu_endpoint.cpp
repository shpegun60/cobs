/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * UartAdapter integrates the STM32 UART driver with a modbus::rtu::Endpoint;
 * handing it any other endpoint type must fail at the adapter's own boundary,
 * not deep inside its template body.
 */
#include "adapters/rtu/UartAdapter.h"

#include <cstddef>
#include <cstdint>
#include <span>

struct FakeHandle {
	struct { uint32_t BaudRate; } Init;
};

template<std::size_t ChunkSize, std::size_t ChunkCount>
class Uart {
public:
	using RxHandler = tiny::delegate<void(std::span<const uint8_t>)>;
	using GapHandler = tiny::delegate<void()>;
	void setRxHandler(RxHandler) noexcept {}
	void setRxGapHandler(GapHandler) noexcept {}
	bool send(std::span<const uint8_t>) noexcept { return true; }
	bool tx_busy() const noexcept { return false; }
	void proceed(uint32_t) noexcept {}
	FakeHandle* instance() const noexcept { return nullptr; }
	uint16_t rx_progress() const noexcept { return 0u; }
};

struct NotAnRtuEndpoint {
	void consume(std::span<const uint8_t>) noexcept {}
	void poll(uint32_t) noexcept {}
};

Uart<256, 4> uart;
NotAnRtuEndpoint endpoint;
modbus::rtu::UartAdapter<Uart<256, 4>, NotAnRtuEndpoint> adapter{uart, endpoint};
