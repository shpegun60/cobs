<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# CRC, COBS, Modbus RTU/TCP + STM32 DMA UART for C++20

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)](https://en.cppreference.com/w/cpp/20)
[![STM32](https://img.shields.io/badge/STM32-DMA%20UART-03234B.svg)](https://www.st.com/stm32)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Production-oriented C++20 libraries for framed serial communication:

- a streaming COBS codec and ownership-safe packet endpoint;
- a protocol-independent CRC8/16/32/64 policy library with bitwise/table modes
  and `NoCrc`;
- a Modbus RTU endpoint with policy-derived integrity framing, explicit
  protocol metadata, and the same ownership model;
- a transport-independent [Modbus TCP endpoint](src/modbus/tcp/README.md),
  framed only by MBAP, with the same storage and ownership API; standard
  NoCrc by default, optional explicit private CRC/size extensions;
- an always-DMA STM32 UART byte transport with zero-copy RX chunks;
- fixed-pool or heap-backed COBS and Modbus storage;
- explicit transport-gap propagation, backpressure, recovery, and statistics;
- host, compile-fail, Cortex-M, benchmark, and real-silicon verification.

Author: [shpegun60](https://github.com/shpegun60)

<!-- toc -->

Contents

- [Choose a protocol](#choose-a-protocol)
- [First working example](#first-working-example)
- [Where things live](#where-things-live)
- [Read by task](#read-by-task)
- [Essential contracts](#essential-contracts)
- [Requirements and validation](#requirements-and-validation)
- [License and author](#license-and-author)

<!-- /toc -->

C++20 protocol libraries with a shared application API: build a Message,
send it through a byte transport, receive an immutable Packet. The transport
and protocol remain independent. Shared storage and CRC policies are selected
at compile time; no virtual transport hierarchy is required.

**Returning after a month? [Почни звідси — карта проєкту українською](doc/START_HERE_UK.md).**

[Documentation index](doc/README.md) · [Examples](doc/EXAMPLES.md) ·
[Qt](doc/QT.md) · [FreeRTOS](doc/FREERTOS.md) · [Build](doc/BUILD.md) ·
[Testing and hardware evidence](doc/TESTING.md)

Older documentation was reorganized, not treated as disposable:
[where each previous section lives, corrections and recovery](doc/DOC_PRESERVATION.md).

## Choose a protocol

| Need | Type | Default useful data | What the library adds |
|---|---|---:|---|
| Your own binary messages over a byte stream | `cobs::Endpoint<>` | 253 bytes | length, CRC16, COBS encoding and delimiter |
| Modbus RTU ADUs supplied whole by another layer | `modbus::rtu::Endpoint<>` | 252 bytes | address, function and CRC16 |
| Modbus RTU over arbitrarily split UART/serial input | RTU Endpoint with a standard Request/Response framer | 252 bytes | same standard RTU bytes; incremental assembly |
| Modbus TCP ADUs in a reliable byte stream | `modbus::tcp::Endpoint<>` | 252 bytes | MBAP, unit and function; no CRC by default |

An endpoint is a codec/owner, not a register database, network stack or
transaction scheduler. The separate Qt RTU client adds request scheduling.
TCP always frames by MBAP; it has no replaceable function framer.

## First working example

The complete [protocols.cpp](doc/examples/protocols.cpp) sends and receives
with all three protocols, writes BE/LE/native fields, checks read failures
and retains a Packet. It needs no board or Qt installation:

```sh
sh doc/examples/build.sh
```

This runs the portable examples plus the STM32/FreeRTOS examples on **host
fakes**, not on hardware. Qt examples use a separate
[event-loop/localhost runner](doc/QT.md#build-and-run).

Typical application types:

```cpp
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "modbus/tcp/Tcp.h"

using Memory = wire::Pool<8, 2>; // eight RX blocks, two TX blocks; not byte sizes
using Cobs = cobs::Endpoint<Memory>;
using Rtu = modbus::rtu::Endpoint<Memory>;
using Tcp = modbus::tcp::Endpoint<Memory>;
```

Use `wire::Heap` instead of `Memory` for dynamic allocation. An explicit
`Format<CRC, N>` sets **N useful `data()` bytes**, never physical buffer size.
Length, headers, CRC and encoding overhead are calculated automatically.
See [default choices and policies](doc/START_HERE_UK.md#політики-без-магії).

## Where things live

```text
src/cobs/           COBS codec, Endpoint, Format, Message, Packet
src/modbus/rtu/     RTU endpoint and optional known-length framing
src/modbus/tcp/     TCP endpoint and MBAP stream parser
src/wire/          shared storage, scalar readers/writers and send result
src/crc/           CRC8/16/32/64, Bitwise/Table, NoCrc, policy codecs
src/uart/          STM32 DMA UART driver, not a protocol parser
src/adapters/      COBS/RTU UART glue, Qt, FreeRTOS wake, STM32 CRC
doc/               user guides, reference, evidence and design history
doc/examples/      runnable cookbook; host scaffolding is labelled explicitly
app/               repository's Qt GUI application, not a library dependency
libs/              external dependencies and vendor headers
```

## Read by task

| Task | Guide |
|---|---|
| Remember what the types and files mean | [Start here, українською](doc/START_HERE_UK.md) |
| Write/read fields and handle Busy, OOM and lifetimes | [User guide](doc/USER_GUIDE.md), [example catalog](doc/EXAMPLES.md) |
| Change protocol with minimal API changes | [API parity](doc/API_PARITY.md), [payload units](doc/PAYLOAD_LIMITS.md) |
| Use QSerialPort, a queued RTU client or QTcpSocket | [Qt recipes](doc/QT.md) |
| Use a sleeping STM32 communication task | [FreeRTOS recipes](doc/FREERTOS.md) |
| Connect a different byte transport | [Integration contracts](doc/INTEGRATION.md) |
| Supply custom storage or a hardware CRC | [Storage](doc/STORAGE.md), [CRC](src/crc/README.md) |
| Understand exact bytes on the wire | [COBS](doc/PROTOCOL.md), [RTU](src/modbus/README.md), [TCP](src/modbus/tcp/README.md) |
| Rebuild or check what was really tested | [Build](doc/BUILD.md), [testing/evidence](doc/TESTING.md) |
| Find an old decision or benchmark | [Complete documentation index](doc/README.md) |

## Essential contracts

- Keep Endpoint/storage alive longer than their Messages and Packets; keep
  transport alive until its last TX borrow ends. Use one execution context.
- Check Message creation and every append. Preserve a pending Message across
  `Busy`; `Sent` means accepted by the transport, not acknowledged by the peer.
- Call the adapter's `proceed()` in application/task context. For a custom
  transport call `poll(now_ms)` to reclaim TX ownership.
- A bare RTU `receive_adu()` requires a complete candidate. UART IDLE and DMA
  chunk size do not guarantee that. Use a framed RTU endpoint for serial streams.
- COBS/RTU/TCP peers must agree on configuration; there is no CRC negotiation.
  Nonstandard Modbus trailers/oversized ADUs are private extensions.
- TCP fails closed on invalid framing or a gap. Start again only on a known
  new stream, normally a new connection.

## Requirements and validation

C++20 and [tiny_delegate](libs/delegate); STM32 UART additionally uses
[SPSC](libs/spsc) and the target HAL/CMSIS. COBS compiles two codec `.cpp`
files; the other protocol/shared modules are header-only. See
[exact include/link commands](doc/BUILD.md).

The latest full-board checkpoint is the
[81-image NUCLEO-H7S3L8 regression](doc/HARDWARE_EXTENSIONS_2026-09-12.md).
It covers the recorded COBS/RTU/TCP, UART fault, FreeRTOS, Heap/OOM and CRC
configurations. TCP-over-UART evidence is not proof of Ethernet/TCP-IP
integration. Host, assembly and hardware evidence are indexed separately in
[Testing](doc/TESTING.md); historical limitations are retained.

## License and author

Project-owned code and documentation: [MIT](LICENSE), © 2026
[shpegun60](https://github.com/shpegun60).
Dependencies retain their own licenses: [third-party notices](THIRD_PARTY_NOTICES.md).

Topics: `cobs` · `modbus-rtu` · `modbus-tcp` · `stm32` · `uart` · `dma` ·
`cpp20` · `embedded` · `freertos` · `qt` · `crc` · `zero-copy-tx`
