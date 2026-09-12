<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# STM32 UART module

<!-- toc -->

Contents

- [What this driver does](#what-this-driver-does)
- [Choose an integration](#choose-an-integration)

<!-- /toc -->

[Documentation](../../doc/README.md) · [User guide](../../doc/USER_GUIDE.md#stm32-uart-quick-start) · [Examples](../../doc/EXAMPLES.md)

## What this driver does

`Uart<ChunkSize, ChunkCount>` owns DMA RX chunks and borrows one immutable TX
span. It knows no COBS, Modbus, CRC, packet allocator or register map. RX/gap
handlers run inside `proceed(now_ms)` in one application context. Tx/error/wake
hooks can run in ISR context. Half-transfer interrupts are disabled on start.

The default is `Uart<128, 8>`. Changing this geometry changes buffering and
interrupt frequency, not a protocol's useful-payload limit. A larger UART
chunk does not guarantee whole Modbus RTU frames.

## Choose an integration

- [Raw driver API, callbacks, diagnostics and baud change](../../doc/USER_GUIDE.md#stm32-uart-quick-start).
- [COBS/RTU with and without adapters](../../doc/INTEGRATION.md).
- [FreeRTOS wake, raw UART and complete tasks](../../doc/FREERTOS.md).
- [Current tests and live-board evidence](../../doc/TESTING.md).

Keep configured HAL handles and DMA-accessible buffers valid. This is a
library usage guide, not a Cube setup generator. The obsolete UartEngine/
circular/IT/RS-485 implementations were [removed and retained in Git](../../doc/LEGACY_REVIEW.md);
they are not supported alternate modes of this driver.
