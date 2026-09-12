<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Adapter map

<!-- toc -->

Contents

- [Pick only the glue your transport needs](#pick-only-the-glue-your-transport-needs)
- [Lifetime and checks](#lifetime-and-checks)

<!-- /toc -->

[Documentation](../../doc/README.md) · [Integration](../../doc/INTEGRATION.md) · [Examples](../../doc/EXAMPLES.md)

## Pick only the glue your transport needs

| Header / type | Purpose | Application guide |
|---|---|---|
| `cobs/UartAdapter.h`: `cobs::UartAdapter` | connect COBS to the STM32 UART | [manual and adapter routes](../../doc/INTEGRATION.md) |
| `rtu/UartAdapter.h`: `modbus::rtu::UartAdapter` | connect RTU; supervise incomplete framed input | [RTU usage](../modbus/README.md) |
| `freertos/FreeRtosWake.h`: `uart::FreeRtosWake` | turn UART events into an owning task's wake | [all wake operations](../../doc/FREERTOS.md#wake-api-every-public-operation) |
| `qt/SerialAdapter.h`: `adapters::qt::SerialAdapter` | QSerialPort + COBS/framed RTU, Qt event loop | [Qt serial](../../doc/QT.md) |
| `qt/RtuClient.h`: `adapters::qt::RtuClient` | queued RTU requests, matching, timeouts/retries | [Qt RTU client/server](../../doc/QT.md#rtu-client-and-server) |
| `stm32/Crc16.h`: `crc::stm32::Crc16` | synchronous programmable-peripheral CRC policy | [hardware CRC use/measurements](../../doc/HEAP_AND_HARDWARE_CRC.md) |

The protocol cores do not depend on these adapters. FreeRtosWake knows the
UART notification, not a protocol. It can be used without UartAdapter or
without any protocol. Neither COBS nor TCP needs RTU's stale-frame timer.
No public TCP socket adapter is provided; the [Qt TCP example](../../doc/QT.md#tcp-over-qtcpsocket)
demonstrates application-owned socket glue with MBAP unchanged.

## Lifetime and checks

Keep driver/port and endpoint alive longer than their adapter. Install handlers
once, in the owning execution context. An adapter's bind does not initialize
hardware, and replacing its underlying RX callback breaks its wiring.

The guides contain full lifecycle, Busy/failure, disconnect and wake recipes.
[Testing](../../doc/TESTING.md) distinguishes host fakes, real Qt,
real-FreeRTOS compile checks and live MCU receipts.
