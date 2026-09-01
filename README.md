<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# COBS framing + STM32 DMA UART for C++20

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C.svg)](https://en.cppreference.com/w/cpp/20)
[![STM32](https://img.shields.io/badge/STM32-DMA%20UART-03234B.svg)](https://www.st.com/stm32)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

Production-oriented C++20 libraries for framed serial communication:

- a streaming COBS codec and ownership-safe packet endpoint;
- an always-DMA STM32 UART byte transport with zero-copy RX chunks;
- fixed-pool or heap-backed COBS storage;
- explicit transport-gap propagation, backpressure, recovery, and statistics;
- host, compile-fail, Cortex-M, benchmark, and real-silicon verification.

Author: [shpegun60](https://github.com/shpegun60)

## What is in this repository?

| Layer | Main include | Responsibility |
|---|---|---|
| COBS application API | [`cobs/Cobs.h`](cobs/Cobs.h) | frames, packet ownership, TX message building, retries, counters |
| COBS storage API | [`cobs/Storage.h`](cobs/Storage.h) | `Format`, `Heap`, `Pool`, custom storage contract |
| Low-level codec | [`cobs/Codec.h`](cobs/Codec.h) | streaming decoder and canonical in-place encoder |
| STM32 UART transport | [`uart/Uart.h`](uart/Uart.h) | DMA RX chunks, borrowed DMA TX, gap/error recovery |
| Integration proof | [`cobs/tests/hardware/h7s`](cobs/tests/hardware/h7s) | real UART + COBS stack on NUCLEO-H7S3L8 |

The layers are intentionally independent. `Uart` transports ordered byte
spans and reports physical gaps. `cobs::Endpoint` owns framing and messages. A
different byte transport can be bound to COBS, and UART can be used without
COBS.

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
- For COBS: [`tiny::delegate`](https://github.com/shpegun60/delegate).
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

`append_native()` is intentionally limited to supported scalar-like values and
writes their native object representation. A portable wire protocol should
still define byte order explicitly and must not serialize padded structs,
`size_t`, `long`, or implementation-sized enums.

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
| `make_message(hint)` | create an empty exclusive TX message and optionally reserve payload capacity |
| `send(message)` | start a frame or return an explicit retry/error result |
| `tx_active()` / `poll()` | observe and reclaim the one transport-borrowed TX block |
| `stats()` / `storage()` | read protocol counters and storage-specific diagnostics |

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
unbinds.

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
| UART host matrix | `sh uart/tests/host/run.sh` | runtime interleavings, errors, recovery, callbacks, baud changes, torture, invalid configs |
| UART port matrix | `sh uart/tests/port/build.sh` | F1/G4/H7RS compile paths, analyzer, probes, hot symbol/stack budgets |
| UART H7S bench | [`uart/tests/bench/README.md`](uart/tests/bench/README.md) | real DMA/IRQ throughput and CPU accounting |
| COBS + UART H7S matrix | [`cobs/tests/hardware/h7s/README.md`](cobs/tests/hardware/h7s/README.md) | independent PC codec, vectors, faults, pools, gaps, stress through 10 Mbaud |

Raw current COBS + UART evidence:

- [baseline audited H7S matrix](cobs/tests/hardware/h7s/results_audited_2026-09-01.jsonl);
- [concise Format/Pool API H7S matrix](cobs/tests/hardware/h7s/results_format_api_2026-09-01.jsonl);
- [UART default 128x8 10 Mbaud run](uart/tests/bench/results_default128x8_10M_audited_2026-09-01.csv);
- [UART chunk-size comparison data](uart/tests/bench/README.md#fresh-audited-run-2026-09-01).

The full post-refactor H7S matrix passed at 115200, 1M, 3M, 6M, and 10M,
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
