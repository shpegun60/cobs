<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Integration: choose the composition, keep the contracts

<!-- toc -->

Contents

- [Supported combinations](#supported-combinations)
- [Common setup and ownership](#common-setup-and-ownership)
- [STM32 with an adapter](#stm32-with-an-adapter)
- [Without an adapter](#without-an-adapter)
  - [Bare RTU requires a whole candidate](#bare-rtu-requires-a-whole-candidate)
  - [Framed RTU can consume fragments](#framed-rtu-can-consume-fragments)
- [Your own byte transport](#your-own-byte-transport)
- [Error handling and shutdown](#error-handling-and-shutdown)
- [Storage, CRC and build dependencies](#storage-crc-and-build-dependencies)

<!-- /toc -->

[Documentation](README.md) · [Почни звідси](START_HERE_UK.md) · [Examples](EXAMPLES.md) · [Qt](QT.md) · [FreeRTOS](FREERTOS.md)

This guide covers all three protocol cores, with and without adapters.
It is the map of responsibilities; the linked platform guides contain the
actual setup/service code in the document itself. A complete program is
labelled as such; a code fragment does not pretend to include platform startup.

## Supported combinations

| Protocol / use | RX entry | Service loop | Ready recipe |
|---|---|---|---|
| COBS + STM32 adapter | installed automatically | `adapter.proceed()` | [cobs_adapter.cpp](examples/cobs_adapter.cpp) |
| Framed RTU + STM32 adapter | installed automatically | `adapter.proceed()` | [rtu_adapter.cpp](examples/rtu_adapter.cpp) |
| Either above + FreeRTOS | IRQ wakes task, parsing stays in task | `wait(adapter); adapter.proceed()` | [complete task](FREERTOS.md#complete-communication-task) |
| COBS manually bound to UART | RX → consume, gap → notify_gap | UART proceed + Endpoint poll | [manual COBS](FREERTOS.md#cobs-with-wake-but-without-uartadapter) |
| Raw UART + optional FreeRTOS wake | application byte handler | UART proceed | [raw UART](FREERTOS.md#raw-uart-without-a-protocol-adapter) |
| Bare RTU, any externally delimited transport | one whole ADU → receive_adu | Endpoint poll | [rtu_direct.cpp](examples/rtu_direct.cpp) |
| Framed RTU, custom transport | arbitrary cuts → consume | transport service + Endpoint poll; own stale policy | [both RTU roles](examples/rtu_framing.cpp) |
| COBS + QSerialPort | SerialAdapter | Qt event loop | [Qt COBS](QT.md#cobs-over-qserialport) |
| RTU + QSerialPort | framed SerialAdapter or RtuClient | Qt event loop | [Qt RTU](QT.md#rtu-client-and-server) |
| TCP + QTcpSocket | arbitrary cuts → consume, MBAP only | application socket glue/event loop | [Qt TCP](QT.md#tcp-over-qtcpsocket) |
| COBS/RTU/TCP + custom byte transport | protocol-specific entry as above | transport service + Endpoint poll | [shared portable example](examples/protocols.cpp) |

There is no general-purpose serial multiplexing layer. Binding COBS and RTU
to the same UART simultaneously does not make bytes distinguishable. Use one
protocol owner per stream, or design an explicit multiplexing protocol above
a suitable framing layer.

## Common setup and ownership

Create objects in an order that makes destruction safe:

1. Long-lived state used by custom CRC/storage and the transport.
2. Endpoint, which owns its storage instance and protocol state.
3. Adapter, if used; it borrows transport and endpoint.
4. Pending Messages and retained Packets, which must die before their endpoint.

With static embedded objects, initialization of the handle/peripheral happens
later; constructing an Endpoint/adapter is not HAL initialization. Both
STM32 adapters expose `bind/unbind/bound` and `proceed()`. Binding does not
transfer ownership of the UART or endpoint.

Use one serialized execution context for Endpoint calls and Packet reference
updates. RX spans from UART expire when the callback returns. Packet spans
remain valid while an owning Packet handle lives. Message is move-only;
`message = {}` abandons it. Packet is shared and has `reset()`.

Delegates are not virtual interfaces. They can hold a callable or bind a
member with `tiny::bind`. Reference/member captures borrow their targets:
the delegate does not extend the target lifetime. Sender/BusyQuery must not
throw or re-enter the same endpoint. The examples keep these operations
bounded and perform parsing after transport events reach application context.

## STM32 with an adapter

The common COBS setup is a firmware fragment; the complete host-checked
version is [cobs_adapter.cpp](examples/cobs_adapter.cpp):

```cpp
#include "uart/Uart.h"
#include "cobs/Cobs.h"
#include "adapters/cobs/UartAdapter.h"

using Serial = Uart<256, 4>;
using Link = cobs::Endpoint<wire::Pool<8, 2>>;
static Serial serial;
static Link endpoint;
static cobs::UartAdapter adapter{serial, endpoint};

bool start(UART_HandleTypeDef& handle) {
    return serial.init(&handle) && adapter.bind();
}
void loop_step() {
    adapter.proceed();
    while (auto packet = endpoint.pop_packet()) {
        // Read packet.data() here, or retain a Packet in this same context.
    }
}
```

Add the driver's HAL callback implementation in exactly one TU, as described
in [UART callback integration](USER_GUIDE.md#callback-integration).
Do not install independent UART RX/gap handlers after adapter.bind(): that
would replace the adapter's wiring. `adapter.proceed()` reads a fresh HAL
millisecond tick and services UART/parser/TX reclamation.

For RTU, replace the protocol and adapter types:

```cpp
#include "modbus/rtu/Rtu.h"
#include "adapters/rtu/UartAdapter.h"

namespace framing = modbus::rtu::framing;
using Link = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Request>>;
static Link endpoint;
static modbus::rtu::UartAdapter adapter{serial, endpoint};
```

This is an alternative declaration block, not a second adapter to bind to the
same serial object. Request means a server's RX; Response means a client's RX.
The adapter handles stale partial RTU frames using UART progress and line rate.
That timing is not strict physical Modbus t1.5/t3.5 framing.

For FreeRTOS the exact same adapter works with:

```cpp
(void)uart::FreeRtosWake::wait(adapter);
adapter.proceed();
```

See [FreeRTOS](FREERTOS.md) for the full task creation, attach order, wake API,
pending replies, error policy and raw/no-adapter variants. There is no COBS
incomplete-frame timer hiding behind this common spelling.

## Without an adapter

Manual binding is useful when the transport is not supported by a ready
adapter or the application already owns its event loop. The responsibilities
do not disappear: connect RX and ordered loss, provide honest TX/busy state,
service the driver, reclaim TX and arrange lifetime-safe detach.

The complete manual COBS wiring and optional FreeRTOS wake appear in
[the no-adapter recipe](FREERTOS.md#cobs-with-wake-but-without-uartadapter).
In a busy loop omit only the wait; still call UART proceed then Endpoint poll.
A bare Endpoint's `poll(now_ms)` currently uses no internal clock state; it
releases a completed TX borrow. It does not service UART or expire RTU input.

### Bare RTU requires a whole candidate

```cpp
modbus::rtu::Endpoint<> endpoint;
// When a separate layer has supplied exactly one complete candidate:
endpoint.receive_adu(whole_adu);
```

This accepts all function numbers without a length table; your application
parses the published function data. It is not a streaming interface.
Neither IDLE nor `Uart<256, 4>` promises a whole ADU: a bridge can insert a
pause inside it; several frames can be delivered together. The complete
[rtu_direct.cpp](examples/rtu_direct.cpp) demonstrates the difference on one
known vector. CRC coincidences exist, so rejection is not a boundary proof.

### Framed RTU can consume fragments

```cpp
namespace framing = modbus::rtu::framing;
using Link = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Request>>;
Link endpoint;
endpoint.consume(first_fragment);
endpoint.consume(next_fragment);
```

Without a transport adapter your application must decide when an incomplete
candidate is abandoned. `expire_incomplete()` is an explicit stale event;
`discard_incomplete()` silently abandons a partial candidate; `notify_gap()`
reports known physical loss. They are not interchangeable with successful
frame completion. For an actual STM32 UART, use the supplied adapter unless
you intend to own and test its progress/deadline responsibilities yourself.
This guide does not replace that proven code with a partial timer pseudocode.

The standard table rejects unknown RX functions and deliberately does not
claim every possible variable Modbus layout (notably 0x2B). Extend it for
private functions with an explicit layout. Known TX layouts are checked;
unknown TX layouts pass through instead of being a universal function filter.
See [RTU framing](../src/modbus/README.md) and
[the private-prefix example](EXAMPLES.md#rtu-framing-and-private-functions).

## Your own byte transport

The contract used by all cores is:

```cpp
using Sender = tiny::delegate<bool(std::span<const uint8_t>)>;
using BusyQuery = tiny::delegate<bool()>;
const bool ok = endpoint.bind(sender, busy);
```

- Sender accepts the entire span or refuses without retaining it. It can copy
  the frame synchronously, or borrow it while BusyQuery remains true.
- BusyQuery also describes transport backpressure; report true if another
  send cannot start. Once it is false and poll runs, the old endpoint block
  may be released immediately.
- Asynchronous transports must accept the whole logical request before
  doing partial writes. Queue/borrow the entire frame, continue partial writes
  internally, and surface failures separately. Do not return false after
  emitting a prefix and then blindly replay the frame.
- One active TX frame belongs to an Endpoint. There is no hidden application
  TX queue. A queueing adapter such as Qt RtuClient documents its additional
  responsibilities separately.

A bounded copying transport, with no peripheral or allocation, is sufficient
for an end-to-end example:

```cpp
struct Transport {
    std::array<uint8_t, 4096> buffer{};
    std::size_t used = 0;
    bool send(std::span<const uint8_t> frame) noexcept {
        if (frame.size() > buffer.size()) { return false; }
        std::copy(frame.begin(), frame.end(), buffer.begin());
        used = frame.size();
        return true;
    }
    bool busy() const noexcept { return false; } // synchronous copy completed
};

Transport transport; // must outlive its delegate binding
cobs::Endpoint<> endpoint;
const bool bound = endpoint.bind(
    cobs::Endpoint<>::Sender{tiny::bind<&Transport::send>(transport)},
    cobs::Endpoint<>::BusyQuery{tiny::bind<&Transport::busy>(transport)});
```

Add `<array>`, `<algorithm>` and the protocol header. This is the same
bounded-copy principle used by [protocols.cpp](examples/protocols.cpp) and
[backpressure.cpp](examples/backpressure.cpp); the complete programs check
creation, append, send, split input, poll and retained Packet lifetime.
TCP uses consume; unframed RTU uses a whole receive_adu candidate. Its own
protocol-specific wire envelope does not belong in the transport.

A datagram/radio/USB API needs its own loss and ordering contract. Knowing
the advertised frame length cannot recover arbitrary silent byte loss.
NoCrc checks no integrity; CRC is not authentication and is not proof that
a guessed boundary was correct. Report known gaps at their stream position.

## Error handling and shutdown

| Event | Required decision |
|---|---|
| Message creation/append fails | stop building or explicitly drop; do not send incomplete application fields |
| Send Busy | retain Message until later; avoid immediate spin loops |
| Send Failed | finalized frame survives; recover transport before deciding whether replay is semantically safe |
| RX allocation refusal | observe stats; release retained Packets, enlarge pool or apply backpressure |
| UART gap | deliver notify_gap in stream order; adapters already do this |
| COBS gap | discard until delimiter; a complete first post-gap frame can be sacrificed |
| RTU stale/gap | explicit incomplete-candidate recovery; whole-candidate guarantees still matter |
| TCP invalid MBAP/gap | close/stop the failed stream; reset only at a known new boundary |
| Task/thread shutdown | end borrows, remove callbacks, release handles, then destroy owners |

`Sent` means local acceptance, not remote delivery. `tx_busy()==false` means
the transport has stopped borrowing memory, not that an application-level
operation succeeded. Retries of commands can duplicate side effects.

To stop a static STM32 application cleanly, first stop issuing new messages,
let/force the real transport finish safely, continue proceed/poll until its
borrow is released, then unbind the adapter. Remove an installed wake before
its task/object disappears. Do not free DMA-visible storage on an unconfirmed
abort. For Qt-specific close/abort/cancellation see [Qt teardown](QT.md#serial-adapter-errors-and-teardown).

## Storage, CRC and build dependencies

`Format<CRC, N>` counts useful data bytes for all protocols. The default RTU
ADU is 256 bytes; TCP 260; COBS uses 253 payload bytes plus its automatic
envelope. Pool counts owners, not bytes. Geometry is compile-time information
for a storage author, not another number a normal user needs to calculate.

[Storage](STORAGE.md) shows the four-operation custom policy and conformance
rules; [CRC](../src/crc/README.md) shows stateless/stateful calculators,
NoCrc and class-owned tables. Use [policies.cpp](examples/policies.cpp) to
see one custom Memory and stateful checksum injected into all three endpoints.

[Build](BUILD.md) lists include roots and source files. [Testing](TESTING.md)
separates examples from sanitizer/compiler/assembly/hardware regression.
