<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# CRC, COBS, Modbus RTU + STM32 DMA UART for C++20

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)](https://en.cppreference.com/w/cpp/20)
[![STM32](https://img.shields.io/badge/STM32-DMA%20UART-03234B.svg)](https://www.st.com/stm32)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Production-oriented C++20 libraries for framed serial communication:

- a streaming COBS codec and ownership-safe packet endpoint;
- a protocol-independent CRC8/16/32/64 policy library with bitwise/table modes
  and `NoCrc`;
- a Modbus RTU endpoint with policy-derived integrity framing, explicit
  protocol metadata, and the same ownership model;
- an always-DMA STM32 UART byte transport with zero-copy RX chunks;
- fixed-pool or heap-backed COBS and Modbus storage;
- explicit transport-gap propagation, backpressure, recovery, and statistics;
- host, compile-fail, Cortex-M, benchmark, and real-silicon verification.

Author: [shpegun60](https://github.com/shpegun60)

## What is in this repository?

| Layer | Main include | Responsibility |
|---|---|---|
| COBS application API | [`cobs/Cobs.h`](cobs/Cobs.h) | frames, packet ownership, TX message building, retries, counters |
| COBS storage API | [`cobs/Storage.h`](cobs/Storage.h) | `Format`, `Heap`, `Pool`, custom storage contract |
| Low-level codec | [`cobs/Codec.h`](cobs/Codec.h) | streaming decoder and canonical in-place encoder |
| Shared scalar I/O | [`wire/Scalar.h`](wire/Scalar.h), [`wire/Read.h`](wire/Read.h) | constrained native/BE/LE scalar representation and stateless bounds-checked readers |
| CRC policy API | [`crc/Crc.h`](crc/Crc.h) | bitwise/table CRC8/16/32/64, wire codecs, custom policy contract, `NoCrc` |
| Modbus RTU API | [`modbus/rtu/Rtu.h`](modbus/rtu/Rtu.h) | burst-delimited RTU ADUs, policy-derived trailer, metadata, packet/message ownership |
| Modbus PDU helpers | [`modbus/Pdu.h`](modbus/Pdu.h) | stateless bounds-checked native/BE/LE function-data readers |
| STM32 UART transport | [`uart/Uart.h`](uart/Uart.h) | DMA RX chunks, borrowed DMA TX, gap/error recovery |
| Integration proof | [`cobs/tests/hardware/h7s`](cobs/tests/hardware/h7s) | real UART + COBS stack on NUCLEO-H7S3L8 |
| Modbus integration proof | [`modbus/rtu/tests/hardware/h7s`](modbus/rtu/tests/hardware/h7s) | real RTU CRC/ownership/pools + UART DMA on NUCLEO-H7S3L8 |

The layers are intentionally independent. `Uart` transports ordered byte
spans and reports physical gaps. `cobs::Endpoint` and
`modbus::rtu::Endpoint` independently own their framing and messages. A
different byte transport can be bound to either endpoint, and UART can be
used without either protocol layer. COBS and Modbus share the stateless
`wire/Scalar.h` and `wire/Read.h` primitives so their native/BE/LE scalar I/O
contracts cannot drift. Modbus additionally selects policies from the
protocol-independent CRC module; COBS does not use that module yet. Neither
protocol depends on the other's framing or ownership types.

```text
RX wire
  -> STM32 UART + DMA
  -> Uart RX chunk
  -> cobs::Endpoint::consume()
  -> immutable cobs::Packet

application payload
  -> cobs::Message
  -> in-place [length + COBS + delimiter]
  -> Uart::send()
  -> DMA borrows the same block
  -> Endpoint::poll() releases it after UART becomes idle
```

## Highlights

- C++20, no virtual transport hierarchy, no exceptions required.
- COBS RX decodes directly into the final packet allocation after its length is
  known.
- COBS TX builds and encodes in one owned block; a failed transport start can
  retry the byte-identical encoded frame.
- `cobs::Packet` is an immutable copyable handle with explicit storage-backed
  lifetime.
- `cobs::Message` is a move-only exclusive TX owner.
- `cobs::Pool` performs deterministic O(1) fixed-block allocation without a
  heap.
- `modbus::rtu::Packet` exposes zero-copy `data()`, `pdu()`, and `adu()` views;
  `Message` adds address, function, and a compile-time CRC/checksum policy.
- RTU keeps one 256-byte physical slab while CRC width determines useful data:
  254 bytes with `NoCrc`, 253/252/250/246 with CRC8/16/32/64.
- `modbus::rtu::Pool<Rx, Tx>` provides fixed 256-byte RTU slabs with independent
  RX/TX ownership quotas and no heap use.
- UART RX DMA writes directly into cache-aligned SPSC chunks; there is no
  intermediate memcpy.
- UART TX has one borrowed in-flight span and no hidden queue.
- RX and TX DMA half-transfer interrupts are explicitly disabled after every
  successful start; only useful completion/IDLE events remain.
- UART discontinuities are delivered in stream order so a decoder never joins
  bytes from opposite sides of a physical loss.
- Compile-verified STM32F1, STM32G4, and STM32H7RS paths; audited H7S silicon
  operation through 10 Mbaud.

## Requirements

- C++20 compiler (`std::span`, concepts, constexpr protocol geometry).
- For COBS and Modbus RTU:
  [`tiny::delegate`](https://github.com/shpegun60/delegate).
- For UART: [`tiny::delegate`](https://github.com/shpegun60/delegate),
  [`spsc`](https://github.com/shpegun60/spsc), STM32 CMSIS, and the target's
  STM32 HAL UART/DMA headers.
- For the checked-in host scripts on Windows: Git Bash and MinGW g++.
- For the recorded embedded matrix: the paths/toolchain described in
  [`doc/BUILD.md`](doc/BUILD.md).

Clone the repository with its dependencies:

```bash
git clone --recurse-submodules https://github.com/shpegun60/cobs.git
cd cobs
```

If it was cloned without submodules:

```bash
git submodule update --init --recursive
```

## COBS quick start

Normal application code includes only:

```cpp
#include "Cobs.h"
```

The shortest endpoint is heap-backed and accepts 255 application bytes in
each direction:

```cpp
cobs::Endpoint<> endpoint;

static_assert(decltype(endpoint)::max_receive_size == 255);
static_assert(decltype(endpoint)::max_send_size == 255);
static_assert(decltype(endpoint)::length_size == 1);
```

### Formats and storage

`Format` numbers are application-body limits. Length bytes, COBS code bytes,
the delimiter, and encoding headroom are calculated internally and do not
reduce the requested payload capacity.

```cpp
cobs::Format<>             // RX 255,  TX 255,  one-byte length
cobs::Format<1024>         // RX 1024, TX 1024, two-byte length
cobs::Format<1024, 64>     // RX 1024, TX 64,   two-byte length
```

The larger directional limit chooses the length-field width for both
directions. Peers must agree on that width.

Built-in storage choices:

```cpp
// Dynamic, exact per-frame allocations; default Format<>.
using DesktopLink = cobs::Endpoint<>;

// Fixed memory, eight RX owners and two TX owners; default Format<>.
using SmallEmbeddedLink = cobs::Endpoint<cobs::Pool<8, 2>>;

// Fixed memory and a symmetric 1024-byte application payload limit.
using Wire = cobs::Format<1024>;
using Memory = cobs::Pool<8, 2, Wire>;
using EmbeddedLink = cobs::Endpoint<Memory>;

// Heap-backed asymmetric protocol.
using AsymmetricLink =
    cobs::Endpoint<cobs::Heap<cobs::Format<1024, 64>>>;
```

The `Pool` block counts remain explicit because they determine static RAM use
and backpressure. The library never invents those quotas.

`Format<255>` still gives all 255 useful payload bytes. Its worst-case wire
storage is larger because it also contains one length byte, COBS overhead, and
the trailing `0x00`. If a complete transport frame must fit a separate hard
256-byte wire buffer, use the sizing functions in `cobs::codec` rather than
subtracting a guessed constant.

See the normative [wire protocol](doc/PROTOCOL.md) and complete
[storage contract](doc/STORAGE.md).

### Bind a byte transport

COBS needs one transactional pair:

- `Sender(span)` starts borrowing a complete wire frame and returns whether it
  accepted that borrow;
- `BusyQuery()` remains true while the transport can still read that memory.

```cpp
struct Transport {
    bool send(std::span<const uint8_t> frame) noexcept;
    bool busy() const noexcept;
};

Transport transport;
cobs::Endpoint<> endpoint;

const bool bound = endpoint.bind(
    cobs::Endpoint<>::Sender{
        tiny::bind<&Transport::send>(transport)},
    cobs::Endpoint<>::BusyQuery{
        tiny::bind<&Transport::busy>(transport)});
```

The sender returns `true` only after it accepts the span. It must keep that
span borrowed until `busy()` returns `false`. Returning `false` means it took
no borrow. Rebinding or unbinding is rejected while a TX block is active.

`tiny::delegate` also supports owned lambdas, explicit `tiny::borrow`, and
member binding. External targets used through `bind`/`borrow` must outlive the
endpoint binding.

### Build and send a message

```cpp
std::array<uint8_t, 3> payload{0x11, 0x00, 0x22};

auto message = endpoint.make_message(payload.size());
if (!message || !message.append_bytes(payload)) {
    // Pool exhausted, allocation failed, or payload exceeds max_send_size.
    return;
}

switch (endpoint.send(message)) {
case cobs::SendResult::Sent:
    // message is now empty; Endpoint owns the block until poll() releases it.
    break;
case cobs::SendResult::Busy:
    // message is unchanged and still belongs to the caller; retry later.
    break;
case cobs::SendResult::Failed:
    // transport refused the start; the same encoded frame is retryable.
    break;
case cobs::SendResult::Unbound:
    // no transport pair is installed; message is unchanged.
    break;
case cobs::SendResult::Invalid:
    // empty message or a message created by another Endpoint instance.
    break;
}
```

`make_message(N)` reserves capacity; it does not set the payload size.
`make_message()` uses a practical hint of up to 32 bytes, while
`make_message(0)` starts with zero capacity and can still grow. Every append is
`[[nodiscard]]`; sending after ignoring a failed append would transmit a safe
but incomplete application message.

Both COBS and Modbus Messages expose the same scalar-writing vocabulary:

```cpp
message.append_native(value); // this target's representation and byte order
message.append_be(value);     // explicit big-endian wire order
message.append_le(value);     // explicit little-endian wire order
message.append_bytes(bytes);  // already serialized bytes
```

Scalar and span overloads are available. Ordered spans encode every element
independently; they never reverse an entire array as one blob. The target byte
order and scalar width are compile-time constants. When requested order equals
`std::endian::native`, the ordered operation delegates to `append_native()`;
otherwise only a fixed 16/32/64-bit swap is instantiated. Targets that permit
unaligned scalar access fold this to direct loads/stores. ARMv6-M and strict-
alignment builds use fully unrolled byte loads/stores instead of an out-of-line
copy helper. There is no runtime endian branch.

These operations are intentionally constrained. They do not serialize padded
structs, `bool`, pointers, or volatile MMIO objects. Portable protocols should
still use fixed-width values and explicitly sized enum underlying types rather
than `size_t`, `long`, or implementation-sized enums.

The receive side uses the matching free-function vocabulary in both protocol
namespaces: `read_native`, `read_be`, `read_le`, and `read_bytes`. `Packet`
stores no mutable parser cursor; the application owns an offset, which makes
independent parsers over one immutable packet safe and explicit.

The selected serializer applies only to application payload/function data.
Library-owned framing never uses native object order: COBS writes and reads its
length prefix explicitly little-endian, while a Modbus RTU CRC policy owns its
trailer through explicit `store/load` operations. The default CRC16 policy is
low-byte-first. COBS code bytes and Modbus address/function are single-byte
fields. Payload append calls cannot reorder any library-owned field.

Call `poll()` regularly. It returns the active TX block to storage after the
transport's busy query becomes false:

```cpp
endpoint.poll();
```

### Receive bytes and packets

Feed any ordered chunking of the byte stream:

```cpp
endpoint.consume(received_bytes);

while (auto packet = endpoint.pop_packet()) {
    process(packet.data()); // immutable application payload only
}
```

Parse typed fields without adding state to `Packet`:

```cpp
std::size_t offset = 0;
uint16_t command = 0;
std::span<const uint8_t> body;

if (!cobs::read_be(packet.data(), offset, command) ||
    !cobs::read_bytes(packet.data(), offset,
                      packet.size() - offset, body)) {
    // malformed application payload
}
```

Every failed read leaves both `offset` and its output unchanged. The same code
shape works for Modbus by replacing `cobs::` with `modbus::`; both names expose
the same `wire/Read.h` functions, not duplicated wrappers.

If the transport knows that one or more bytes were physically lost, report it
at the exact stream position:

```cpp
endpoint.notify_gap();
```

The current partial frame is discarded, and delivery resumes only after a
delimiter re-establishes framing. Never hide a UART/DMA overrun from the COBS
layer: surrounding bytes can accidentally form structurally valid COBS.

### COBS ownership and execution rules

- An endpoint must outlive every `Packet` and `Message` created from it.
- Do not destroy an endpoint while `tx_active()` is true or the transport may
  still read its frame.
- Send a message only through the exact endpoint instance that created it.
- Mutating endpoint calls are externally serialized; COBS v1 contains no
  internal thread/RTOS locking.
- Packet reference counting is deliberately non-atomic and stays in one
  execution domain.
- A transport becoming idle proves only that it released the memory, not that
  a remote peer received or acknowledged the frame.

The detailed state/lifetime model is in
[`doc/ARCHITECTURE.md`](doc/ARCHITECTURE.md).

### COBS diagnostics and pool pressure

`stats()` returns a value snapshot. It never exposes mutable receiver or
transmitter state:

```cpp
const cobs::Stats counters = endpoint.stats();

// RX outcome counters:
// counters.rx.frames_delivered
// counters.rx.frames_lost
// counters.rx.allocation_failure
// counters.rx.malformed
// counters.rx.oversize
// counters.rx.length_mismatch
// counters.rx.resyncs

// TX outcome counters:
// counters.tx.frames_sent
// counters.tx.send_refused_busy
// counters.tx.send_failed
```

For a pool-backed endpoint, the read-only storage view also exposes current
capacity and allocator diagnostics:

```cpp
using Link = cobs::Endpoint<cobs::Pool<8, 2>>;
Link link;

const std::size_t free_rx_blocks = link.storage().rx_available();
const std::size_t free_tx_blocks = link.storage().tx_available();
const auto& rx_pool = link.storage().rx_stats(); // exhausted, rejected
const auto& tx_pool = link.storage().tx_stats(); // exhausted, rejected
```

`exhausted` is normal backpressure: no block was available. `rejected` means
storage refused an invalid release such as a foreign pointer or double free;
it should remain zero in a correct integration.

### COBS API at a glance

| Call | Use |
|---|---|
| `bind(sender, busy)` / `unbind()` | install or remove one transactional transport pair while no TX block is active |
| `consume(bytes)` | feed ordered bytes from any transport chunking |
| `notify_gap()` | mark a known physical discontinuity at its exact stream position |
| `has_packet()` / `pop_packet()` | inspect or take the next immutable received packet |
| `read_native` / `read_be` / `read_le` / `read_bytes` | parse packet data with an application-owned cursor and a strong failure guarantee |
| `make_message(hint)` | create an empty exclusive TX message and optionally reserve payload capacity |
| `send(message)` | start a frame or return an explicit retry/error result |
| `tx_active()` / `poll()` | observe and reclaim the one transport-borrowed TX block |
| `stats()` / `storage()` | read protocol counters and storage-specific diagnostics |

## Modbus RTU quick start

Modbus RTU deliberately uses the same ownership verbs as COBS, but it has its
own namespace and wire framing:

```cpp
#include "modbus/rtu/Rtu.h"

using Memory = modbus::rtu::Pool<8, 2>;
using Modbus = modbus::rtu::Endpoint<Memory>;

Modbus link;
```

`Pool<8, 2>` means eight simultaneously owned RX packets and two TX
messages/transport borrows. The heap-backed convenience form is simply:

```cpp
modbus::rtu::Endpoint<> link;
```

The second Endpoint template argument selects a complete CRC/checksum policy
at compile time: result type, wire width, calculation, and store/load codec.
The portable table-free CRC-16/MODBUS implementation remains the default; its
faster alias uses one private 512-byte flash table:

```cpp
using FastModbus = modbus::rtu::Endpoint<
    Memory, modbus::rtu::crc::Table>;
```

The independent [`crc/Crc.h`](crc/Crc.h) module also supplies CRC8/32/64,
bitwise/table variants, reusable integer wire codecs, and `NoCrc`. A custom
stateful policy can retain a hardware peripheral handle and can intentionally
implement another checksum. The library performs no semantic validation: it
uses the same object for RX and TX exactly as supplied. See the full
[CRC policy guide](modbus/README.md#crc-policy-and-compile-time-rtu-format).

Create an RTU request by passing address and function once; the library adds
the selected trailer and advertises only policy-derived useful data capacity:

```cpp
auto request = link.make_message(1u, 0x03u);
if (!request ||
    !request.append_be(uint16_t{0x0010u}) ||
    !request.append_be(uint16_t{2u})) {
    return;
}

const modbus::SendResult result = link.send(request);
```

Receive packets expose immutable metadata and three zero-copy views:

```cpp
while (auto packet = link.pop_packet()) {
    use(packet.address(), packet.function(), packet.data());
    inspect_pdu(packet.pdu()); // function + data
    inspect_adu(packet.adu()); // address + PDU + selected trailer
}
```

For the supplied STM32 adapter, use `Uart<256, N>` and forward one complete
ReceiveToIdle burst to `receive_adu()`. Unlike streaming COBS `consume()`, an
RTU receive call is exactly one candidate ADU; it is not arbitrary chunking.

```cpp
using Serial = Uart<256, 4>;

uart.setRxHandler(Serial::RxHandler{
    [](std::span<const uint8_t> candidate) noexcept {
        link.receive_adu(candidate);
    }});
uart.setRxGapHandler(Serial::GapHandler{
    []() noexcept { link.notify_gap(); }});
```

With the default CRC16, the maximum PDU is 253 bytes: one function byte plus up
to 252 function-data bytes. Address and two CRC bytes make the RTU ADU exactly
256 bytes. The physical limit remains 256 for every policy; `NoCrc` recovers
the two trailer bytes and exposes 254 function-data bytes. Alternate policies
are private wire formats, not standard Modbus RTU. This v1 adapter uses a
continuous UART burst as its physical boundary;
it does not claim strict software t1.5/t3.5 timing. Read the full
[Modbus usage guide](modbus/README.md) and canonical
[Modbus architecture](modbus/ARCHITECTURE.md) before integration.

## STM32 UART quick start

`Uart<ChunkSize, ChunkCount>` is a header-defined STM32 HAL byte transport.
Defaults are based on the measured H7S sweep:

```cpp
using Serial = Uart<128, 8>;
```

The object contains its RX chunk memory. Give it static lifetime and place it
in DMA-accessible RAM. On Cortex-M7, do not place it in DTCM. Either retain the
default cache maintenance or place the object in an MPU non-cacheable DMA
region and compile consistently with `UART_ENGINE_DCACHE_MAINTENANCE=0`.

### Callback integration

With the default `UART_ENGINE_INTERNAL_CALLBACKS_ON=1` and HAL registered
callbacks disabled, define this macro in exactly one `.cpp` before including
the header:

```cpp
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
```

Other translation units include `Uart.h` normally. Alternatives are:

- `USE_HAL_UART_REGISTER_CALLBACKS=1`: `init()` registers callbacks through
  HAL;
- `UART_ENGINE_INTERNAL_CALLBACKS_ON=0`: application-owned HAL callbacks
  forward to `uart::detail::Registry::onRxEvent`, `onTxCplt`, and `onError`.

All `UART_ENGINE_*` configuration macros must have identical values in every
translation unit that includes the header.

Library-level configuration, defined before `#include "Uart.h"`:

| Macro | Default | Meaning |
|---|---:|---|
| `UART_ENGINE_MAX_INSTANCES` | `4` | capacity of the static callback registry; there is no heap |
| `UART_ENGINE_DCACHE_MAINTENANCE` | auto | `1` on a core reporting D-cache, otherwise `0`; set `0` only for a consistently non-cacheable DMA region |
| `UART_ENGINE_INTERNAL_CALLBACKS_ON` | `1` | emit or suppress application-global HAL callback forwarding |
| `UART_ENGINE_CHECK_PERIOD_MS` | `200` | period of the cold HAL/DMA liveness audit inside `proceed()` |
| `UART_ENGINE_FAIL_THRESHOLD` | `3` | consecutive bad audits before recovery |
| `UART_ENGINE_HAS_RXEVENT_TYPE` | `1` | set `0` only for an older HAL lacking `HAL_UARTEx_GetRxEventType()` |

These are compile-time policy choices, not per-object runtime settings. The
callback mode also depends on the HAL's `USE_HAL_UART_REGISTER_CALLBACKS`
setting; the three supported combinations are compile-tested.

### Initialize and service UART

```cpp
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"

static Uart<> serial;

bool start_uart(UART_HandleTypeDef* huart) noexcept
{
    serial.setRxHandler(Uart<>::RxHandler{
        [](std::span<const uint8_t> bytes) noexcept {
            consume_bytes(bytes);
        }});

    serial.setRxGapHandler(Uart<>::GapHandler{
        []() noexcept {
            reset_stream_decoder();
        }});

    serial.setErrorHandler(Uart<>::ErrorHandler{
        [](uint32_t hal_error) noexcept {
            record_uart_error(hal_error);
        }});

    serial.setTxHandler(Uart<>::TxHandler{
        [](bool ok) noexcept {
            record_tx_result(ok);
        }});

    return serial.init(huart);
}

void main_loop()
{
    for (;;) {
        const uint32_t now = HAL_GetTick();
        serial.proceed(now);
        // application work
    }
}
```

`init()` is one-shot and transactional. It validates the handle, linked RX/TX
DMA channels, byte widths, DMA modes, states, UART payload width, full-duplex
mode, and callback registry before arming reception. A failed initialization
does not leave a half-owned peripheral and may be retried.

Supported byte framing is exactly:

- 8-bit word length with no parity; or
- 9-bit word length with even/odd parity, yielding eight application bits.

The UART must be `TX_RX`; hardware half-duplex is rejected because RX remains
armed during TX. RX and TX use distinct normal-mode DMA channels. See the
complete initialization proof in
[`doc/UART_PARANOID_AUDIT.md`](doc/UART_PARANOID_AUDIT.md).

### UART receive contract

- DMA writes into a claimed SPSC chunk.
- ISR work publishes its size and immediately arms the next chunk.
- `proceed(now_ms)` invokes `RxHandler` and `GapHandler` in thread context.
- The received span is valid only for the duration of `RxHandler`; copy it or
  finish consuming it before returning.
- Call `proceed()` frequently from exactly one loop/execution context.
- A pool overrun switches DMA to a drop buffer, records `rx_overrun`, and later
  announces one ordered gap before trustworthy bytes resume.

### UART transmit contract

```cpp
if (!serial.tx_busy()) {
    const bool accepted = serial.send(bytes);
}
```

`send()` starts DMA directly over caller-owned memory. The span must be
non-empty, no larger than 65,535 bytes, and remain alive and unchanged until
`tx_busy()` becomes false. There is no internal TX queue. `TxHandler` reports
the terminal result, normally from an ISR; watchdog recovery can report it
from `proceed()`.

### Runtime baud change

```cpp
if (!serial.tx_busy()) {
    const bool changed = serial.setBaudRate(1'000'000);
}
```

`setBaudRate()` is thread-context only. It refuses a live TX, stops and
restarts RX, preserves/restores supported FIFO configuration, and deliberately
announces an RX gap so upper framing cannot merge bytes received at different
line rates.

### UART diagnostics

```cpp
const Uart<>::Stats stats = serial.stats();

// stats.rx_overrun
// stats.rx_errors
// stats.tx_errors
// stats.restarts
```

These are lightweight diagnostic counters, not synchronization or billing
accounting. `stats()` returns an IRQ-guarded coherent snapshot, while updates
remain intentionally cheap plain increments.

### UART API at a glance

| Call | Use / execution rule |
|---|---|
| `init(huart)` | one-shot exclusive bind; retry is allowed only after a failed initialization |
| `setRxHandler(...)` | install the thread-context borrowed-span consumer |
| `setRxGapHandler(...)` | install the thread-context ordered-loss notification |
| `setTxHandler(...)` | receive terminal TX success/failure, normally from ISR context |
| `setErrorHandler(...)` | receive the HAL error mask from ISR context |
| `proceed(now_ms)` | drain RX and run recovery from exactly one loop context |
| `send(bytes)` / `tx_busy()` | start and track one borrowed DMA TX span |
| `setBaudRate(baud)` | transactional thread-context line-rate change with a deliberate RX gap |
| `stats()` / `instance()` | read diagnostics or the bound HAL handle |

## Complete UART + COBS composition

This is the intended embedded arrangement. UART stays a byte transport; COBS
receives byte chunks and ordered gap notifications.

```cpp
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "Cobs.h"

class SerialStack final {
public:
    using Serial = Uart<128, 8>;
    using Wire = cobs::Format<1024>;
    using Memory = cobs::Pool<8, 2, Wire>;
    using Link = cobs::Endpoint<Memory>;

    bool init(UART_HandleTypeDef* huart) noexcept
    {
        uart_.setRxHandler(Serial::RxHandler{
            tiny::bind<&SerialStack::on_rx>(*this)});
        uart_.setRxGapHandler(Serial::GapHandler{
            tiny::bind<&SerialStack::on_gap>(*this)});

        if (!uart_.init(huart)) {
            return false;
        }

        return link_.bind(
            Link::Sender{tiny::bind<&Serial::send>(uart_)},
            Link::BusyQuery{tiny::bind<&Serial::tx_busy>(uart_)});
    }

    bool queue(std::span<const uint8_t> payload) noexcept
    {
        if (pending_) {
            return false; // application-level queue/backpressure policy
        }

        auto message = link_.make_message(payload.size());
        if (!message || !message.append_bytes(payload)) {
            return false;
        }

        pending_ = static_cast<Link::Message&&>(message);
        return true;
    }

    void proceed(uint32_t now_ms) noexcept
    {
        uart_.proceed(now_ms); // invokes on_rx/on_gap in stream order
        link_.poll();          // releases a completed UART TX block

        while (auto packet = link_.pop_packet()) {
            handle_packet(packet.data());
        }

        if (pending_) {
            const cobs::SendResult result = link_.send(pending_);
            if (result == cobs::SendResult::Unbound ||
                result == cobs::SendResult::Invalid) {
                pending_ = Link::Message{};
            }
            // Sent clears pending_. Busy/Failed retain it for a later retry.
        }
    }

    [[nodiscard]] const Link& link() const noexcept { return link_; }
    [[nodiscard]] const Serial& uart() const noexcept { return uart_; }

private:
    void on_rx(std::span<const uint8_t> bytes) noexcept
    {
        link_.consume(bytes);
    }

    void on_gap() noexcept
    {
        link_.notify_gap();
    }

    static void handle_packet(std::span<const uint8_t> payload) noexcept;

    Serial uart_{};
    Link link_{};
    Link::Message pending_{};
};

// Static lifetime also guarantees that DMA storage outlives the peripheral.
static SerialStack serial_stack;
```

Production code normally adds application-specific error/terminal callbacks,
message scheduling, and packet dispatch. It should not add another framing
buffer between these layers.

The exact implementation used for real-silicon testing is
[`cobs/tests/hardware/h7s/cobs_bench.cpp`](cobs/tests/hardware/h7s/cobs_bench.cpp).

## Wire protocol

Every engine frame is:

```text
COBS( little_endian_body_length | application_body ) 00
```

The length counts only application-body bytes. Empty application packets are
valid. Bare delimiters are synchronization no-ops. The encoder is canonical;
the decoder accepts structurally valid non-canonical COBS.

Length width:

```text
max(RX limit, TX limit) <= 255  -> 1 byte
otherwise                      -> 2 bytes
```

Both limits must fit `uint16_t`. Peers with different length widths are wire
incompatible even for a one-byte body. For vectors, size arithmetic, malformed
behavior, retry identity, and the gap state machine, read
[`doc/PROTOCOL.md`](doc/PROTOCOL.md).

## Build integration

### qmake

COBS provides a reusable fragment:

```qmake
include(path/to/cobs/cobs.pri)
```

It enables C++20, publishes headers, adds the delegate include path, and
compiles `Decoder.cpp` and `Encoder.cpp` exactly once. External directory
layouts can set `COBS_DELEGATE_DIR` before including it.

The independent CRC policy module is header-only:

```qmake
include(path/to/crc/crc.pri)
```

Modbus RTU is also header-only and provides its own fragment, which includes
the CRC dependency automatically:

```qmake
include(path/to/modbus/rtu/rtu.pri)
```

External directory layouts can set `MODBUS_DELEGATE_DIR` before including it.

### CMake or another build system

There is no generated library binary to match. Add the two non-template codec
sources and include roots to your target:

```cmake
target_sources(app PRIVATE
    path/to/cobs/Decoder.cpp
    path/to/cobs/Encoder.cpp)

target_include_directories(app PRIVATE
    path/to/cobs
    path/to/libs/delegate)

target_compile_features(app PRIVATE cxx_std_20)
```

For Modbus RTU, add the repository and delegate include roots; there are no
protocol `.cpp` files:

```cmake
target_include_directories(app PRIVATE
    path/to/repository
    path/to/libs/delegate)

target_compile_features(app PRIVATE cxx_std_20)
```

For UART, add the header include roots plus your STM32 HAL/CMSIS paths:

```cmake
target_include_directories(firmware PRIVATE
    path/to/uart
    path/to/libs/delegate
    path/to/libs/spsc
    path/to/libs/spsc/src)
```

`Uart.h` includes the Cube-generated `main.h`, which must expose the target HAL
and `UART_HandleTypeDef`. Compile one callback implementation translation unit
as described above.

Exact recorded commands and qmake consumer instructions are in
[`doc/BUILD.md`](doc/BUILD.md).

A small runnable downstream-style COBS application is checked in at
[`cobs/tests/qmake_consumer/main.cpp`](cobs/tests/qmake_consumer/main.cpp). It
binds a transport, sends through both `Heap` and `Pool`, loops the wire frame
back into RX, validates the packet, polls ownership, reads statistics, and
parses native/BE/LE fields through the public reader facade before unbinding.

The corresponding Modbus consumer is
[`modbus/rtu/tests/qmake_consumer/main.cpp`](modbus/rtu/tests/qmake_consumer/main.cpp).

## Verification

The repository distinguishes host behavior, compile-time contracts,
cross-target code generation, benchmarks, and real hardware evidence.

| Verification | Command / evidence | What it checks |
|---|---|---|
| COBS host suite | `sh cobs/tests/run.sh` | public headers, compile-fail boundaries, decoder/encoder, storage, ownership, endpoint, debug and `-DNDEBUG` |
| Exhaustive codec oracle | included in `cobs/tests/run.sh` | 960,800 decoder streams and 177,146 encoder/headroom cases |
| COBS qmake consumer | `sh cobs/tests/qmake_consumer/run.sh` | real downstream include/link/use path for Heap and Pool |
| Cortex-M COBS layout | `sh cobs/tests/check_arm_layout.sh` | ARM object layout assertions |
| COBS benchmarks | `sh cobs/tests/bench/run.sh` | codec and complete Endpoint hot paths |
| CRC host suite | `sh crc/tests/run.sh` | known CRC8/16/32/64 models, independent random oracles, both methods, codecs, custom policy and `NoCrc` under sanitizers and O3 |
| Cortex-M CRC emission | `sh crc/tests/check_arm_codegen.sh` | all CRC8/16/32/64 loops are helper-free on both CPU byte orders; unused tables emit zero bytes, selected tables emit one read-only object, codecs are branch/call-free, `NoCrc` folds away |
| Shared scalar/API host oracle | `sh wire/tests/run.sh` | exhaustive scalar values, reader facade identity, COBS/Modbus public API parity, intentional protocol differences, sanitizers and O3/LTO |
| GCC strict/LTO consumers | `MATRIX_TAG=<compiler> CXX=<g++> sh wire/tests/check_gcc_matrix.sh` | real COBS/Modbus consumers and API parity under strict alias/alignment/bounds warnings plus `-fshort-enums`/`-funsigned-char` scalar proof |
| Cortex-M endian hot path | `sh wire/tests/check_arm_hotpath.sh` | little- and big-endian ARM builds prove compile-time selection: native order is direct, opposite order uses REV/REV16, and neither calls a helper |
| Cortex-M codegen matrix | `sh wire/tests/check_arm_codegen_matrix.sh` | 96 scalar, 60 protocol and 30 COBS objects across M0/M0+/M3/M4/M7/M23/M33/M55, plus Bitwise/Table references, Os/O2/O3, endian and strict-alignment variants |
| Modbus CRC layout/codegen | `sh modbus/rtu/tests/check_arm_crc_codegen.sh` | default Endpoint emits no table; Table emits one private 512-byte read-only lookup; empty policies add no RAM |
| Modbus RTU host suite | `sh modbus/rtu/tests/run.sh` | headers, compile-fail boundaries, every CRC width/method, custom three-byte and hardware policies, `NoCrc`, derived geometry, storage, ownership, endpoint and fuzz properties |
| Modbus qmake consumer | `sh modbus/rtu/tests/qmake_consumer/run.sh` | downstream header-only use with Heap, Pool and Table policy |
| Modbus + UART fake HAL | `sh modbus/rtu/tests/run_uart_integration.sh` | short IDLE ADU, exact 256-byte TC ADU, gaps, recovery, and DMA TX borrow |
| Cortex-M Modbus layout | `sh modbus/rtu/tests/check_arm_layout.sh` | ARM object layout and static RAM assertions |
| Modbus + UART H7S matrix | [`modbus/rtu/tests/hardware/h7s/README.md`](modbus/rtu/tests/hardware/h7s/README.md) | independent PC CRC oracle, Bitwise/Table A/B, exact 256-byte ADUs, corruptions, pools, recovery and stress at 115200/1M |
| UART host matrix | `sh uart/tests/host/run.sh` | runtime interleavings, errors, recovery, callbacks, baud changes, torture, invalid configs |
| UART port matrix | `sh uart/tests/port/build.sh` | F1/G4/H7RS compile paths, analyzer, probes, hot symbol/stack budgets |
| UART H7S bench | [`uart/tests/bench/README.md`](uart/tests/bench/README.md) | real DMA/IRQ throughput and CPU accounting |
| COBS + UART H7S matrix | [`cobs/tests/hardware/h7s/README.md`](cobs/tests/hardware/h7s/README.md) | independent PC codec, vectors, faults, pools, gaps, stress through 10 Mbaud |

Raw current hardware evidence:

- [baseline audited H7S matrix](cobs/tests/hardware/h7s/results_audited_2026-09-01.jsonl);
- [concise Format/Pool API H7S matrix](cobs/tests/hardware/h7s/results_format_api_2026-09-01.jsonl);
- [UART default 128x8 10 Mbaud run](uart/tests/bench/results_default128x8_10M_audited_2026-09-01.csv);
- [UART chunk-size comparison data](uart/tests/bench/README.md#fresh-audited-run-2026-09-01);
- [Modbus accepted 115200/1M matrix](modbus/rtu/tests/hardware/h7s/results_audited_2026-09-02.jsonl);
- [Modbus universal-scalar 115200/1M matrix](modbus/rtu/tests/hardware/h7s/results_scalar_api_final_2026-09-02.jsonl);
- [Modbus final paranoid `-Os` 115200/1M matrix](modbus/rtu/tests/hardware/h7s/results_paranoid_final_2026-09-02.jsonl);
- [Modbus `-O2` silicon run](modbus/rtu/tests/hardware/h7s/results_paranoid_o2_2026-09-02.jsonl);
- [Modbus `-O3` + LTO silicon run](modbus/rtu/tests/hardware/h7s/results_paranoid_o3_lto_2026-09-02.jsonl);
- [Modbus post-extraction CRC library 115200/1M matrix](modbus/rtu/tests/hardware/h7s/results_crc_library_2026-09-05.jsonl);
- [Modbus Bitwise/Table CRC A/B](modbus/rtu/tests/hardware/h7s/results_crc_policy_2026-09-05.jsonl);
- [Modbus 3M UART-IDLE boundary probe](modbus/rtu/tests/hardware/h7s/results_high_baud_probe_2026-09-02.jsonl).

The full post-refactor COBS H7S matrix passed at 115200, 1M, 3M, 6M, and 10M,
including physical gap/recovery tests. Its extended 10 Mbaud/window-7 run
delivered 61,611 exact frames and 19,133,016 application bytes in 30 seconds,
with zero unexpected UART or COBS loss. See the evidence README for the exact
acceptance rules and limitations of those measurements.

## Documentation map

Read active documents in this order:

1. [Architecture](doc/ARCHITECTURE.md) — public surfaces, ownership, data flow,
   lifetimes, and execution domains.
2. [Wire protocol](doc/PROTOCOL.md) — normative frame grammar and peer
   compatibility.
3. [Storage contract](doc/STORAGE.md) — `Heap`, `Pool`, sizing, and custom
   storage implementations.
4. [Build and verification](doc/BUILD.md) — exact commands and toolchains.
5. [Detailed COBS engine record](doc/COBS_ENGINE.md) — state machines, overlap
   proof, allocation rationale, and rejected alternatives.
6. [COBS paranoid audit](doc/COBS_PARANOID_AUDIT.md) — correctness and hot-path
   evidence.
7. [UART paranoid audit](doc/UART_PARANOID_AUDIT.md) — HAL contracts, races,
   recovery, cache/DMA rules, and code-generation evidence.
8. [Refactor plan and decision history](doc/COBS_REFACTOR_PLAN.md) — locked
   architectural decisions and completed phases.
9. [CRC policy guide](crc/README.md) - built-in models, wire codecs, custom
   hardware policies, table emission, and `NoCrc`.
10. [Modbus RTU usage](modbus/README.md) — public API, UART binding, storage,
   diagnostics, tests, and the burst-framing limitation.
11. [Modbus architecture](modbus/ARCHITECTURE.md) — RTU ownership invariants
    and the separate future `modbus::tcp` boundary.

Files under [`doc/old`](doc/old) are preserved historical designs and legacy
code. They are not the current API and should not be copied into new
integrations.

## Common questions

### Why is the default COBS body limit 255 instead of 256?

One byte represents values `0..255`. The protocol uses `0` for an empty body,
so the largest directly representable one-byte length is 255. A 256-byte body
uses a two-byte length field. Service bytes are allocated in addition to the
declared body capacity.

### Does `Pool<8, 2>` mean byte sizes?

No. It means eight RX ownership blocks and two TX ownership blocks. Its omitted
third parameter is `Format<>` (255/255). Use
`Pool<8, 2, Format<1024>>` for a 1024-byte symmetric body limit.

### Why is there no COBS or UART TX queue?

Queueing policy belongs to the application: drop, retry, prioritize, or store
elsewhere. Both layers expose honest busy/exhaustion states without hiding
latency, memory, or ownership.

### Is UART half-transfer enabled?

No. STM32 HAL enables DMA HT interrupts on start, but this driver exposes no
half-transfer event and explicitly disables HT after every successful RX and
TX start. The code paths still mention HT because disabling it is required.

### Can `Packet` cross RTOS tasks?

Not under the current v1 contract. Its reference count is intentionally plain
and cheap. Keep copies/releases in one execution domain or design a separate
atomic ownership policy.

### What should happen after an RX overrun?

UART reports a gap in stream order. Forward it to `Endpoint::notify_gap()`.
COBS discards through the next delimiter and then resumes; do not treat the
first bytes after an unknown physical loss as a complete trusted frame.

## License

Project-owned code and documentation are licensed under the
[MIT License](LICENSE), copyright © 2026
[shpegun60](https://github.com/shpegun60).

Git submodules and vendor-derived STM32 configuration files retain their own
licenses. See [third-party notices](THIRD_PARTY_NOTICES.md).
