<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Integration: choose the composition, keep the contracts

<!-- toc -->

Contents

- [Supported combinations](#supported-combinations)
- [Common setup and ownership](#common-setup-and-ownership)
- [STM32 with an adapter](#stm32-with-an-adapter)
  - [RTU adapter: split servicing and lifecycle](#rtu-adapter-split-servicing-and-lifecycle)
- [Without an adapter](#without-an-adapter)
  - [Bare RTU requires a whole candidate](#bare-rtu-requires-a-whole-candidate)
  - [Framed RTU can consume fragments](#framed-rtu-can-consume-fragments)
  - [Complete manual RTU + UART client](#complete-manual-rtu--uart-client)
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
| Framed RTU + UART, no protocol adapter | manually installed RX/gap/send/busy | one-request budget + UART proceed + Endpoint poll | [complete manual client](#complete-manual-rtu--uart-client) |
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
static Link g_endpoint;
static cobs::UartAdapter adapter{serial, g_endpoint};

bool start(UART_HandleTypeDef& handle) {
    return serial.init(&handle) && adapter.bind();
}
void loop_step() {
    adapter.proceed();
    while (auto packet = g_endpoint.pop_packet()) {
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
static Link g_endpoint;
static modbus::rtu::UartAdapter adapter{serial, g_endpoint};
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

### RTU adapter: split servicing and lifecycle

Ordinary code uses `adapter.proceed()`. If your loop needs separate timing
scopes around UART work, use this complete alternative from
[rtu_adapter.cpp](examples/rtu_adapter.cpp), built also with `DOC_SPLIT=1`:

<!-- example: examples/rtu_adapter.cpp#rtu-split-service -->
```cpp
void service_transport() noexcept
{
#if DOC_SPLIT
	const uint32_t now = HAL_GetTick(); // one clock sample for all four calls
	adapter.prepare(now);              // baud refresh and DMA-progress snapshot
	serial.proceed(now);               // deliver queued RX/gaps BEFORE judging expiry
	adapter.finish(now);               // judge the incomplete frame after delivery
	g_endpoint.poll(now);              // finish() alone does not reclaim TX
#else
	adapter.proceed();                 // ordinary application: the same steps internally
#endif
}
```
<!-- /example -->

Call `service_transport()`, then drain packets and send pending replies as
usual. Do not also call `adapter.proceed()` in that iteration. All four calls
use the same millisecond clock/sample; no synthetic tick mixed with HAL time.
`prepare` must precede UART delivery, `finish` must follow it, and `poll`
must remain last. Otherwise a continuation already queued at its deadline can
be expired before delivery, or a completed TX block can remain unreclaimed.

| STM32 adapter operation | Contract |
|---|---|
| `bind()`, `bound()`, `unbind()` | same names for COBS/RTU; driver initialized first; failed bind/unbind changes neither wiring nor binding |
| `proceed()` / `proceed(now)` | service UART, protocol-specific recovery and endpoint TX reclamation; automatic or explicit HAL-millisecond domain |
| `deadline_in_ms()` / `(now)` | delay until RTU incomplete-frame expiry; COBS always returns `no_deadline` |
| RTU `prepare(now)` / `finish(now)` | advanced split of proceed, with UART delivery between them and endpoint poll afterward |
| RTU `on_rx(bytes)` / `on_gap()` | entry points installed by bind; custom callers must preserve service order; empty RX does not restart a deadline |
| RTU `baud()`, `full_chunk_ms()`, `deadline_armed()` | inspect the adapter's current timing state; baud is refreshed during prepare/proceed |
| RTU `chunk_time_ms(baud)` | pure geometry-based duration helper, rounded up; returns 0 for baud 0 (bind still requires a real nonzero rate) |

One adapter owns a UART's RX/gap callbacks at a time. There is no ownership
registry preventing a second adapter from overwriting the first one's callbacks.
Explicitly unbind the old one before installing another. An active endpoint TX
blocks unbind even if DMA has just completed: service poll first. Destruction
detaches RX but does not make an outstanding TX borrow disappear; keep the
driver and endpoint alive and service that borrow to completion before destroying them.

Detaching RTU silently discards its incomplete candidate. Detaching COBS uses
the counted `notify_gap()` recovery: discard through the next delimiter, which
can sacrifice the first new frame after rebind. Already queued Packets survive
both. Neither detach is a promise that the next byte starts a valid frame.

RTU's adapter allows 5 ms after a partial published chunk, or one full chunk's
transfer time plus 5 ms after a full chunk, using 12 bits/character and the
current UART baud. At expiry it checks **new** DMA progress, not merely a
nonzero counter. The cookbook checks queued continuation at the deadline,
empty input and frozen progress under both combined and split servicing.
This is transport-aware stale recovery, not strict physical t1.5/t3.5 timing.

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
The complete manual example below deliberately uses a different, application
transaction budget; it does not approximate the UART adapter's silence rule.

The standard table rejects unknown RX functions and deliberately does not
claim every possible variable Modbus layout (notably 0x2B). Extend it for
private functions with an explicit layout. Known TX layouts are checked;
unknown TX layouts pass through instead of being a universal function filter.
See [RTU framing](../src/modbus/README.md) and
[the private-prefix example](EXAMPLES.md#rtu-framing-and-private-functions).

### Complete manual RTU + UART client

[rtu_uart_direct.cpp](examples/rtu_uart_direct.cpp) wires the actual STM32 UART
to a Response-framed endpoint, without `UartAdapter`. The demo reads one
holding register (unit `0x11`, FC03, register 0, count 1). It has one pending
Message, no hidden queue, no automatic retry, and a **1000-ms total request
budget** including Busy, TX and response. Choose a budget for your actual
baud, maximum frame, peer processing time and scheduling latency.

The following three consecutive blocks contain the entire firmware-side
implementation. Leave `DOC_HOST` undefined: that section of the source is
only the executable fake-HAL tests. For bare metal, `DOC_WAKE` defaults to 0.

<!-- example: examples/rtu_uart_direct.cpp#manual-rtu-types -->
```cpp
#define UART_ENGINE_IMPLEMENT // exactly one TU in the firmware
#include "uart/Uart.h"
#include "modbus/rtu/Rtu.h"
#include <algorithm>

#ifndef DOC_WAKE
#define DOC_WAKE 0
#endif
#if DOC_WAKE
#include "adapters/freertos/FreeRtosWake.h"
#endif

namespace manual_rtu {
namespace framing = modbus::rtu::framing;
using Serial = Uart<256, 4>;
using Link = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Response>>;

// Place the UART AND its borrowed Pool TX buffers in DMA-accessible RAM.
static Serial serial;
static Link endpoint;
static Link::Message pending;
#if DOC_WAKE
static uart::FreeRtosWake wake;
#endif
enum class Result { Idle, Pending, Waiting, Completed, Timeout, Gap, Failed, ResponseError };
static Result result = Result::Idle;
static bool initialized = false;
static uint16_t value = 0;
static uint32_t started_ms = 0;
constexpr uint32_t request_budget_ms = 1000u; // Busy + TX + response, not RTU t1.5/t3.5

bool active() noexcept { return result == Result::Pending || result == Result::Waiting; }
```
<!-- /example -->

<!-- example: examples/rtu_uart_direct.cpp#manual-rtu-start -->
```cpp
bool start(UART_HandleTypeDef& handle) noexcept
{
    if (!serial.init(&handle) || !endpoint.bind(
            Link::Sender{tiny::bind<&Serial::send>(serial)},
            Link::BusyQuery{tiny::bind<&Serial::tx_busy>(serial)})) { return false; }
    serial.setRxHandler(Serial::RxHandler{[](std::span<const uint8_t> bytes) noexcept {
        if (active()) { endpoint.consume(bytes); } // arbitrary cuts, never receive_adu(chunk)
    }});
    serial.setRxGapHandler(Serial::GapHandler{[]() noexcept {
        endpoint.notify_gap();
        if (active()) { pending = {}; result = Result::Gap; }
    }});
    initialized = true;
    return true;
}

bool begin_read() noexcept
{
    if (!initialized || active() || endpoint.tx_active()) { return false; }
    // Caller establishes a new request boundary; this cannot identify future late responses.
    endpoint.discard_incomplete();
    while (auto packet = endpoint.pop_packet()) {} // drop previously queued unsolicited packets
    pending = endpoint.make_message(0x11u, 0x03u);
    if (!pending || !pending.append_be(uint16_t{0}) || !pending.append_be(uint16_t{1})) {
        pending = {}; result = Result::Failed; return false;
    }
    started_ms = HAL_GetTick();
    result = Result::Pending;
    return true;
}
```
<!-- /example -->

<!-- example: examples/rtu_uart_direct.cpp#manual-rtu-service -->
```cpp
void step() noexcept
{
#if DOC_WAKE
    uint32_t wait_ms = 50u; // periodic UART health service, also when no request is active
    if (active()) {
        const uint32_t elapsed = HAL_GetTick() - started_ms;
        wait_ms = elapsed >= request_budget_ms ? 0u
            : std::min(wait_ms, request_budget_ms - elapsed);
    }
    (void)uart::FreeRtosWake::wait(wait_ms); // called only by the task attached to wake
#endif
    const uint32_t now = HAL_GetTick(); // fresh after waking; one tick for this iteration
    serial.proceed(now);               // RX/gap callbacks execute here, not in ISR
    endpoint.poll(now);                // release only TX memory no longer borrowed

    // Drain already published responses BEFORE applying the application's deadline.
    while (auto packet = endpoint.pop_packet()) {
        if (result != Result::Waiting || packet.address() != 0x11u ||
            (packet.function() != 0x03u && packet.function() != 0x83u)) { continue; }
        std::size_t offset = 0;
        uint8_t count = 0;
        uint16_t received = 0;
        const bool valid = packet.function() == 0x03u &&
            modbus::read_be(packet.data(), offset, count) && count == 2u &&
            modbus::read_be(packet.data(), offset, received) && offset == packet.size();
        if (valid) { value = received; }
        result = valid ? Result::Completed : Result::ResponseError;
        endpoint.discard_incomplete(); // no second response belongs to this one-request demo
    }

    if (active() && static_cast<uint32_t>(now - started_ms) >= request_budget_ms) {
        pending = {};                 // cancels an unsent Message; not an accepted DMA borrow
        endpoint.expire_incomplete();  // explicit transaction abandonment, not a silence guess
        result = Result::Timeout;
    }
    if (result == Result::Pending) {
        const auto sent = endpoint.send(pending);
        if (sent == wire::SendResult::Sent) { result = Result::Waiting; }
        else if (sent != wire::SendResult::Busy) {
            pending = {}; result = Result::Failed; // no automatic physical-error retry
        }
    }
}

bool stop() noexcept
{
    // Keep calling step until the transport has provably released its borrow.
    if (serial.tx_busy() || endpoint.tx_active() || !endpoint.unbind()) { return false; }
    serial.setRxHandler({});
    serial.setRxGapHandler({});
#if DOC_WAKE
    serial.setWakeHandler({});
#endif
    pending = {};
    endpoint.discard_incomplete();
    while (auto packet = endpoint.pop_packet()) {}
    initialized = false;
    result = Result::Idle;
    return true;
}
} // namespace manual_rtu
```
<!-- /example -->

Use this code in one application TU. After platform configuration, check
`manual_rtu::start(huart3)`, then check `manual_rtu::begin_read()`. Call
`manual_rtu::step()` continuously from the same owner loop. Once `result` is
`Completed`, `value` holds the validated register. The other terminal results
are Timeout, Gap, Failed or ResponseError (including Modbus exceptions).
Observe/save the result before beginning another request. Initialization is
one-shot; `stop()` detaches the protocol, not a UART deinit/reinit facility.

Important boundaries of this explicit policy:

- A response is assembled from arbitrary chunks; UART IDLE never means ADU end.
- A complete response already published to the driver is processed before the
  deadline verdict. This is a cooperative application budget, not a physical
  timestamp proving when the last UART bit arrived.
- Empty input, repeated partial bytes and unpublished DMA progress do not extend
  the fixed transaction budget. No separate per-fragment stale timer is used.
- Timeout cancels pending work and explicitly expires a partial response, but
  does not free a live TX borrow. Keep servicing until ownership is released;
  `stop()` and the next request refuse an outstanding endpoint TX.
- Incoming bytes outside an active request are ignored. Starting a request
  clears parser/queued data, **not unpublished DMA bytes or future late replies**.
  RTU has no transaction ID: a late reply with the same address/function may
  match a new request. After loss/timeout, establish your application's recovery
  boundary before retrying; this demo does not prove at-most-once execution.
- `Failed` is not replayed automatically. Even a transport-refused send may
  have put a prefix on the physical line; the application decides recovery.

For the sleeping-task variant and its startup sequence, see
[manual RTU with FreeRTOS wake](FREERTOS.md#rtu-with-wake-but-without-uartadapter).
Both variants execute the same deadline, gap, Busy, exception, tick-wrap and
DMA-lifetime checks. The ready UART adapter remains the option for its own
progress-aware stale policy; this example documents a genuinely different
application-owned policy instead of duplicating its implementation.

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
