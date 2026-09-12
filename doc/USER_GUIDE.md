<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# User guide: protocols and STM32 UART

<!-- toc -->

Contents

- [What is in this repository?](#what-is-in-this-repository)
- [Highlights](#highlights)
- [Requirements](#requirements)
- [COBS quick start](#cobs-quick-start)
  - [Formats and storage](#formats-and-storage)
  - [Bind a byte transport](#bind-a-byte-transport)
  - [Build and send a message](#build-and-send-a-message)
  - [Receive bytes and packets](#receive-bytes-and-packets)
  - [COBS ownership and execution rules](#cobs-ownership-and-execution-rules)
  - [COBS diagnostics and pool pressure](#cobs-diagnostics-and-pool-pressure)
  - [COBS API at a glance](#cobs-api-at-a-glance)
- [Modbus RTU quick start](#modbus-rtu-quick-start)
- [STM32 UART quick start](#stm32-uart-quick-start)
  - [Callback integration](#callback-integration)
  - [Initialize and service UART](#initialize-and-service-uart)
  - [Event-driven servicing under an RTOS](#event-driven-servicing-under-an-rtos)
  - [UART receive contract](#uart-receive-contract)
  - [UART transmit contract](#uart-transmit-contract)
  - [Runtime baud change](#runtime-baud-change)
  - [UART diagnostics](#uart-diagnostics)
  - [UART API at a glance](#uart-api-at-a-glance)
- [Complete UART + COBS composition](#complete-uart--cobs-composition)
- [Wire protocol](#wire-protocol)
- [Build integration](#build-integration)
- [Verification](#verification)
- [Documentation map](#documentation-map)
- [Common questions](#common-questions)
  - [Why is the default COBS body limit 255 instead of 256?](#why-is-the-default-cobs-body-limit-255-instead-of-256)
  - [Does Pool<8, 2> mean byte sizes?](#does-pool-mean-byte-sizes)
  - [Why is there no COBS or UART TX queue?](#why-is-there-no-cobs-or-uart-tx-queue)
  - [Is UART half-transfer enabled?](#is-uart-half-transfer-enabled)
  - [Can Packet cross RTOS tasks?](#can-packet-cross-rtos-tasks)
  - [What should happen after an RX overrun?](#what-should-happen-after-an-rx-overrun)
- [License](#license)

<!-- /toc -->

[Documentation index](README.md) · [Start here (Українською)](START_HERE_UK.md) · [Examples](EXAMPLES.md)

This is the detailed user guide. Start with the short repository README or
[the return-to-project guide](START_HERE_UK.md) if you need a map, not internals.
For Qt use [QT.md](QT.md); for a sleeping communication task use
[FREERTOS.md](FREERTOS.md). All three protocol cores use the same storage,
writer/reader vocabulary and ownership rules.

## What is in this repository?

`src/` is the stack (`wire/`, `crc/`, `cobs/`, `modbus/`, `uart/`) plus
`adapters/`, the glue that knows both a transport and an endpoint
(`cobs/UartAdapter.h`, `rtu/UartAdapter.h`, `freertos/FreeRtosWake.h`, `qt/SerialAdapter.h` and
`qt/RtuClient.h`); `libs/` the third-party
dependencies, `app/` the Qt host application, `doc/` the documentation of the
whole repository. Include paths are `src`-relative.

| Layer | Main include | Responsibility |
|---|---|---|
| COBS application API | [`src/cobs/Cobs.h`](../src/cobs/Cobs.h) | frames, packet ownership, TX message building, retries, counters |
| Shared storage API | [`src/wire/Storage.h`](../src/wire/Storage.h) | `Heap`, `Pool`, three-value Geometry, protocol-blind custom memory |
| Low-level codec | [`src/cobs/Codec.h`](../src/cobs/Codec.h) | streaming decoder and canonical in-place encoder |
| Shared scalar I/O | [`src/wire/Scalar.h`](../src/wire/Scalar.h), [`src/wire/Read.h`](../src/wire/Read.h) | constrained native/BE/LE scalar representation and stateless bounds-checked readers |
| CRC policy API | [`src/crc/Crc.h`](../src/crc/Crc.h) | bitwise/table CRC8/16/32/64, wire codecs, custom policy contract, `NoCrc` |
| Modbus RTU API | [`src/modbus/rtu/Rtu.h`](../src/modbus/rtu/Rtu.h) | burst-delimited RTU ADUs, policy-derived trailer, metadata, packet/message ownership |
| Modbus TCP API | [`src/modbus/tcp/Tcp.h`](../src/modbus/tcp/Tcp.h) | MBAP stream assembly, transaction/unit/function metadata; no socket or TCP/IP dependency |
| Modbus PDU helpers | [`src/modbus/Pdu.h`](../src/modbus/Pdu.h) | stateless bounds-checked native/BE/LE function-data readers |
| STM32 UART transport | [`src/uart/Uart.h`](../src/uart/Uart.h) | DMA RX chunks, borrowed DMA TX, gap/error recovery |
| Integration proof | [`src/cobs/tests/hardware/h7s`](../src/cobs/tests/hardware/h7s) | real UART + COBS stack on NUCLEO-H7S3L8 |
| Modbus integration proof | [`src/modbus/rtu/tests/hardware/h7s`](../src/modbus/rtu/tests/hardware/h7s) | real RTU CRC/ownership/pools + UART DMA on NUCLEO-H7S3L8 |

The layers are intentionally independent. `Uart` transports ordered byte
spans and reports physical gaps. `cobs::Endpoint` and
`modbus::rtu::Endpoint` / `modbus::tcp::Endpoint` independently own their framing and messages. A
different byte transport can be bound to any endpoint, and UART can be
used without a protocol layer. COBS and Modbus share the stateless
`src/wire/Scalar.h` and `src/wire/Read.h` primitives so their native/BE/LE scalar I/O
contracts cannot drift. All three select policies from the
protocol-independent CRC module and bind the same memory specifications from
`src/wire/Storage.h` to their own computed geometry. Neither
protocol depends on the other's framing or ownership types.

```text
RX wire
  -> STM32 UART + DMA
  -> Uart RX chunk
  -> cobs::Endpoint::consume()
  -> immutable cobs::Packet

application payload
  -> cobs::Message
  -> in-place [length + CRC + COBS + delimiter]
  -> Uart::send()
  -> DMA borrows the same block
  -> Endpoint::poll(now_ms) releases it after UART becomes idle
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
- `wire::Pool` performs deterministic O(1) fixed-block allocation without a
  heap.
- `modbus::rtu::Packet` exposes zero-copy `data()`, `pdu()`, and `adu()` views;
  `Message` adds address, function, and a compile-time CRC/checksum policy.
- RTU defaults to 252 useful data bytes (256-byte ADU with CRC16);
  `Format<Crc, MaxData>` configures data capacity, like COBS and TCP.
  If an application deliberately wants a fixed 256-byte ADU with another CRC,
  it must choose MaxData explicitly (254 for NoCrc, for example). Merely
  changing the CRC policy keeps the default 252 DATA bytes.
- `wire::Pool<Rx, Tx>` provides geometry-sized slabs with independent
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
- For COBS, Modbus RTU and Modbus TCP:
  [`tiny::delegate`](https://github.com/shpegun60/delegate).
- For UART: [`tiny::delegate`](https://github.com/shpegun60/delegate),
  [`spsc`](https://github.com/shpegun60/spsc), STM32 CMSIS, and the target's
  STM32 HAL UART/DMA headers.
- For the checked-in host scripts on Windows: Git Bash and MinGW g++.
- For the recorded embedded matrix: the paths/toolchain described in
  [`doc/BUILD.md`](BUILD.md).

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
#include "cobs/Cobs.h"
```

The shortest endpoint is heap-backed and accepts 253 application bytes with CRC16 in
each direction:

```cpp
cobs::Endpoint<> endpoint;

static_assert(decltype(endpoint)::max_receive_size == 253);
static_assert(decltype(endpoint)::max_send_size == 253);
static_assert(decltype(endpoint)::length_size == 1);
```

### Formats and storage

`Format` numbers are useful application-payload limits. CRC, length and COBS code bytes,
the delimiter, and encoding headroom are calculated internally and do not
reduce the requested payload capacity.

```cpp
cobs::Format<>                            // CRC16, payload 253/253, H1
cobs::Format<crc::Crc16Table>               // same wire, faster table calculation
cobs::Format<crc::Crc16Bitwise, 1024>       // payload 1024/1024, H2
cobs::Format<crc::Crc16Bitwise, 1024, 64>   // asymmetric payload, H2
cobs::Format<crc::NoCrc, 255>               // explicit legacy v1, H1
```

The larger directional payload limit plus CRC width chooses the length width
for both directions. Peers must agree on that width and checksum semantics.
There is no version marker or automatic detection; an old NoCrc receiver can
deliver a new frame's trailer as application data. See [migration](PROTOCOL.md#compatibility).

Built-in storage choices:

```cpp
// Dynamic, exact per-frame allocations; default Format<>.
using DesktopLink = cobs::Endpoint<>;

// Fixed memory, eight RX owners and two TX owners; default Format<>.
using SmallEmbeddedLink = cobs::Endpoint<wire::Pool<8, 2>>;

// Fixed memory and a symmetric 1024-byte application payload limit.
using Wire = cobs::Format<crc::Crc16Bitwise, 1024>;
using Memory = wire::Pool<8, 2>;
using EmbeddedLink = cobs::Endpoint<Memory, Wire>;

// Heap-backed asymmetric protocol.
using AsymmetricLink =
    cobs::Endpoint<wire::Heap, cobs::Format<crc::Crc16Bitwise, 1024, 64>>;
```

The `Pool` block counts remain explicit because they determine static RAM use
and backpressure. The library never invents those quotas.

`Format<crc::Crc16Bitwise, 255>` gives all 255 useful payload bytes. Its body
is 257 bytes, so storage includes two length bytes, the CRC, COBS overhead, and
the trailing `0x00`. If a complete transport frame must fit a separate hard
256-byte wire buffer, use the sizing functions in `cobs::codec` rather than
subtracting a guessed constant.

See the normative [wire protocol](PROTOCOL.md) and complete
[storage contract](STORAGE.md).

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
    // message is now empty; Endpoint owns the block until poll(now_ms) releases it.
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

Readers accept `enum class` (prefer an explicit underlying type such as
`uint16_t`), but reject every unscoped enum at compile time. C++20 cannot
portably tell a fixed unscoped enum from an unfixed one, for which arbitrary
wire bytes can produce undefined behavior. For an unscoped enum, read the
underlying integer and validate its value before converting. Writers remain
unchanged. Readers check byte bounds, not whether an enum value has a name.

The selected serializer applies only to application payload/function data.
Library-owned framing never uses native object order: COBS writes and reads its
length prefix explicitly little-endian, while a Modbus RTU CRC policy owns its
trailer through explicit `store/load` operations. The default CRC16 policy is
low-byte-first. COBS code bytes and Modbus address/function are single-byte
fields. Payload append calls cannot reorder any library-owned field.

For a manually bound transport call `poll(now_ms)` regularly with the application's millisecond tick; none of the three cores currently uses this value for timing. A ready adapter calls poll for you. RTU stale-frame timing belongs to its transport adapter, not to `Endpoint::poll()`. The endpoint returns the active TX block to storage after the
transport's busy query becomes false:

```cpp
endpoint.poll(HAL_GetTick());
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
the same `src/wire/Read.h` functions, not duplicated wrappers.

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
- Mutating endpoint calls are externally serialized; the protocol core contains no
  internal thread/RTOS locking.
- Packet reference counting is deliberately non-atomic and stays in one
  execution domain.
- A transport becoming idle proves only that it released the memory, not that
  a remote peer received or acknowledged the frame.

The detailed state/lifetime model is in
[`doc/ARCHITECTURE.md`](ARCHITECTURE.md).

### COBS diagnostics and pool pressure

`stats()` returns a value snapshot. It never exposes mutable receiver or
transmitter state:

```cpp
const cobs::Stats counters = endpoint.stats();

// RX outcome counters:
// counters.rx.frames_received
// counters.rx.frames_lost
// counters.rx.allocation_failure
// counters.rx.malformed
// counters.rx.oversize
// counters.rx.length_mismatch
// counters.rx.crc_errors
// counters.rx.resyncs

// TX outcome counters:
// counters.tx.frames_sent
// counters.tx.send_refused_busy
// counters.tx.send_failed
```

For a pool-backed endpoint, the read-only storage view also exposes current
capacity and allocator diagnostics:

```cpp
using Link = cobs::Endpoint<wire::Pool<8, 2>>;
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
| `tx_active()` / `poll(now_ms)` | observe and reclaim the one transport-borrowed TX block; RTU stale-frame supervision runs in its adapter |
| `stats()` / `storage()` | read protocol counters and storage-specific diagnostics |

## Modbus RTU quick start

Modbus RTU deliberately uses the same ownership verbs as COBS, but it has its
own namespace and wire framing:

```cpp
#include "modbus/rtu/Rtu.h"

using Memory = wire::Pool<8, 2>;
using Modbus = modbus::rtu::Endpoint<Memory>;

Modbus link;
```

`Pool<8, 2>` means eight simultaneously owned RX packets and two TX
messages/transport borrows. The heap-backed convenience form is simply:

```cpp
modbus::rtu::Endpoint<> link;
```

The second Endpoint argument is `modbus::rtu::Format<Crc, MaxData>`.
It selects the integrity policy and useful function-data capacity. The library
adds address, function and CRC automatically, just as TCP adds MBAP/function/CRC.
The portable table-free CRC-16/MODBUS implementation remains the default; its
Table option uses one private 512-byte flash table:

```cpp
using FastModbus = modbus::rtu::Endpoint<
    Memory, modbus::rtu::Format<crc::Crc16Table>>;
```

The independent [`src/crc/Crc.h`](../src/crc/Crc.h) module also supplies CRC8/32/64,
bitwise/table variants, reusable integer wire codecs, and `NoCrc`. A custom
stateful policy can retain a hardware peripheral handle and can intentionally
implement another checksum. The library performs no semantic validation: it
uses the same object for RX and TX exactly as supplied. See the full
[CRC policy guide](../src/modbus/README.md#crc-policy-and-compile-time-rtu-format).

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

For arbitrary UART chunks, use a framed endpoint and the supplied adapter.
Direction describes RX: Request for a server, Response for a client. The
example below is a separate setup fragment, not an extra binding on `link`:

```cpp
using Serial = Uart<256, 4>;
namespace framing = modbus::rtu::framing;
using Stream = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Request>>;
static Serial serial;
static Stream g_endpoint;
static modbus::rtu::UartAdapter adapter{serial, g_endpoint};
// Include adapters/rtu/UartAdapter.h. Check serial.init(&huart3), then adapter.bind().
// In the owner loop: adapter.proceed(); then drain g_endpoint.pop_packet().
```

With the default CRC16, the maximum PDU is 253 bytes: one function byte plus up
to 252 function-data bytes. Address and two CRC bytes make the RTU ADU exactly
256 bytes. Selecting a different CRC leaves the configured DATA capacity
unchanged and resizes storage for its trailer. `Format<crc::NoCrc>` therefore
still exposes 252 data bytes, in a smaller 254-byte ADU. Specify a different
data limit explicitly when needed. See [shared payload limits](PAYLOAD_LIMITS.md).
Non-Modbus checksum semantics or actual ADUs above 256 are private exchanges;
CRC16 Table or an equivalent hardware calculator remain compatible. Bare
`Endpoint<>` instead requires one whole external candidate per `receive_adu()`.
UART IDLE and a 256-byte chunk do not prove that boundary. The known-length
framer is not strict physical t1.5/t3.5 timing. Read the full
[Modbus usage guide](../src/modbus/README.md) and canonical
[Modbus architecture](../src/modbus/ARCHITECTURE.md) before integration.

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
#include "uart/Uart.h"
```

Other translation units include `Uart.h` normally. Alternatives are:

- `USE_HAL_UART_REGISTER_CALLBACKS=1`: `init()` registers callbacks through
  HAL;
- `UART_ENGINE_INTERNAL_CALLBACKS_ON=0`: application-owned HAL callbacks
  forward to `uart::detail::Registry::onRxEvent`, `onTxCplt`, and `onError`.

All `UART_ENGINE_*` configuration macros must have identical values in every
translation unit that includes the header.

When you already own global HAL callbacks, use the explicit forwarding variant
below. Set `UART_ENGINE_INTERNAL_CALLBACKS_ON=0` consistently for every TU
(for example as a target definition), and do not also enable registered UART
callbacks. These are routing calls, not places to parse packets or allocate:

```cpp
#include "uart/Uart.h"
extern "C" void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* handle, uint16_t count)
{
    uart::detail::Registry::onRxEvent(handle, count);
}
extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* handle)
{
    uart::detail::Registry::onTxCplt(handle);
}
extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* handle)
{
    uart::detail::Registry::onError(handle);
}
```

Library-level configuration, defined before `#include "uart/Uart.h"`:

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
#include "uart/Uart.h"

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
[`doc/UART_PARANOID_AUDIT.md`](UART_PARANOID_AUDIT.md).

### Event-driven servicing under an RTOS

`proceed()` is a thread-context call and the RX handler runs inside it, so a
task that sleeps never sees a chunk until something wakes it. The driver's
`WakeHandler` is that something: raised from the RX event, TX completion and
error ISRs after the driver's state is final, carrying no data and knowing
no scheduler. `src/adapters/freertos/FreeRtosWake.h` turns it into a FreeRTOS task
notification:

```cpp
#include "adapters/freertos/FreeRtosWake.h"

static uart::FreeRtosWake wake;                  // takes no task: safe before the task exists

void communicationTask(void*)
{
    if (!wake.attach(serial, xTaskGetCurrentTaskHandle()) ||
        !serial.init(&huart3) || !adapter.bind()) {
        for (;;) { vTaskSuspend(nullptr); } // replace with the application's failure policy
    }
    for (;;) {
        (void)uart::FreeRtosWake::wait(adapter);
        adapter.proceed();                      // fresh platform tick, uart -> endpoint
        while (auto packet = link.pop_packet()) { handle(packet); }
    }
}
```

Several interrupts before the task runs coalesce into one wake. The wait has
two bounds. The fallback is periodic service for work no UART event
announces (the driver's health audit and application work). `wait(adapter)`
automatically respects an incomplete RTU frame's deadline and caps the sleep
at 50 ms; `wait(adapter, 20u)` selects a shorter application fallback. COBS
has no incomplete-frame timer. No caller timestamp or deadline calculation
is needed: the STM32 adapter owns its HAL clock, and `proceed()` reads it
again after waking. Explicit `proceed(now_ms)` and `deadline_in_ms(now_ms)`
remain available for custom schedulers and deterministic tests. The finite
wait conversion is overflow-safe and does not use `pdMS_TO_TICKS()`; a
deadline shorter than one kernel tick can require nonblocking service.
Choose tick resolution appropriate to the timing budget. `wait()` acts on the
calling task and must be called only by the attached one, whose notification
index 0 then belongs to the UART wake. The `FromISR` call requires the USART
and DMA interrupts not to be logically more urgent than the kernel's syscall
ceiling: on STM32, the HAL/CMSIS number given to `HAL_NVIC_SetPriority()`
must be `>= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (the unshifted
form; `configMAX_SYSCALL_INTERRUPT_PRIORITY` is the same limit shifted into
the register's form and is not the number to compare against). The
communication task must be the only one touching the driver, the endpoint
and its packets. Earlier hook-cost measurements are historical evidence in
[the UART audit](UART_PARANOID_AUDIT.md), not a total ISR budget. A bare-metal loop that calls
`proceed()` unconditionally leaves the handler unset. See [FreeRTOS](FREERTOS.md)
for the complete task, all wake methods and versions without an adapter.

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
| `setWakeHandler(...)` | optional: be told from ISR context that `proceed()` has work (RX chunk or gap queued, TX finished, error pending); the hook an RTOS task sleeps on |
| `rx_progress()` | bytes DMA has taken into the chunk it still owns (not yet published by IDLE/TC); a thread-context snapshot a transport adapter uses to tell a silent line from a chunk still filling |
| `proceed(now_ms)` | drain RX and run recovery from exactly one loop context |
| `send(bytes)` / `tx_busy()` | start and track one borrowed DMA TX span |
| `setBaudRate(baud)` | transactional thread-context line-rate change with a deliberate RX gap |
| `stats()` / `instance()` | read diagnostics or the bound HAL handle |

## Complete UART + COBS composition

This is the intended embedded arrangement. UART stays a byte transport; COBS
receives byte chunks and ordered gap notifications. The other arrangements —
RTU through its matching `UartAdapter`, FreeRTOS on top, either protocol without an adapter, a
desktop or TCP transport — are enumerated in
[`doc/INTEGRATION.md`](INTEGRATION.md).

```cpp
#define UART_ENGINE_IMPLEMENT
#include "uart/Uart.h"
#include "cobs/Cobs.h"
#include "adapters/cobs/UartAdapter.h"

class SerialStack final {
public:
    using Serial = Uart<128, 8>;
    using Wire = cobs::Format<crc::Crc16Bitwise, 1024>;
    using Memory = wire::Pool<8, 2>;
    using Link = cobs::Endpoint<Memory, Wire>;

    bool init(UART_HandleTypeDef* huart) noexcept
    {
        return uart_.init(huart) && adapter_.bind();
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

    void proceed() noexcept
    {
        adapter_.proceed(); // platform clock, drains UART RX/gaps, reclaims TX

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
    static void handle_packet(std::span<const uint8_t> payload) noexcept;

    Serial uart_{};
    Link link_{};
    cobs::UartAdapter<Serial, Link> adapter_{uart_, link_};
    Link::Message pending_{};
};

// Static lifetime also guarantees that DMA storage outlives the peripheral.
static SerialStack serial_stack;
```

Production code normally adds application-specific error/terminal callbacks,
message scheduling, and packet dispatch. It should not add another framing
buffer between these layers.

COBS and RTU share the builder, readers, ownership result and basic adapter
lifecycle. See [the end-user API parity contract](API_PARITY.md) for the
side-by-side API, migration notes and the deliberate protocol differences.
The same `uart::FreeRtosWake` works with either adapter when a FreeRTOS task
sleeps between events; it is unnecessary in a bare-metal polling loop.
The [live API/task follow-up](HARDWARE_API_PARITY_2026-09-12.md) records
the full protocol regression and the two-line loop on a real FreeRTOS kernel.

The exact implementation used for real-silicon testing is
[`src/cobs/tests/hardware/h7s/cobs_bench.cpp`](../src/cobs/tests/hardware/h7s/cobs_bench.cpp).

## Wire protocol

Every engine frame is:

```text
COBS( little_endian_body_length | payload | CRC(payload) ) 00
```

The length counts payload plus trailer, not the length field itself. Empty application packets are
valid. Bare delimiters are synchronization no-ops. The encoder is canonical;
the decoder accepts structurally valid non-canonical COBS.

Length width:

```text
max(RX payload limit, TX payload limit) + CRC width <= 255 -> 1 byte
otherwise                      -> 2 bytes
```

Both payload-plus-trailer limits must fit `uint16_t`. Peers with different length widths are wire
incompatible even for a one-byte body. For vectors, size arithmetic, malformed
behavior, retry identity, and the gap state machine, read
[`doc/PROTOCOL.md`](PROTOCOL.md).

## Build integration

Use the repository `src` include root, so includes match the folder names:
`"cobs/Cobs.h"`, `"modbus/rtu/Rtu.h"`, `"modbus/tcp/Tcp.h"`,
`"uart/Uart.h"`. Add `libs/delegate`; UART also needs `libs/spsc`,
`libs/spsc/src` and your configured HAL/CMSIS include directories.

Only COBS needs protocol sources: compile `src/cobs/Encoder.cpp` and
`src/cobs/Decoder.cpp` once. RTU, TCP, wire and CRC are header-only.
UART's callback implementation macro belongs to exactly one translation unit.

qmake consumers include the appropriate `src/cobs/cobs.pri`,
`src/modbus/rtu/rtu.pri` or `src/modbus/tcp/tcp.pri` fragment; Qt serial
integration additionally uses `src/adapters/qt/qt.pri`.

See [build commands, CMake and toolchains](BUILD.md), the
[runnable examples](EXAMPLES.md), and the [Qt build recipe](QT.md#build-and-run).

## Verification

See [Testing and evidence](TESTING.md) for reproducible host/compiler/Qt
commands and the distinction between examples, regression suites, real hardware
and network tests. The latest full-board checkpoint is the
[81-image H7S repeat](HARDWARE_EXTENSIONS_2026-09-12.md).
Historical performance figures remain tied to their recorded configuration;
they are not universal CPU budgets or fresh measurements from this guide.

## Documentation map

The [documentation index](README.md) groups all maintained guides, module
references, test receipts and historical design plans. Use the
[example catalog](EXAMPLES.md) to choose by task instead of guessing a file.
Old designs were [removed from the checkout and retained in Git](LEGACY_REVIEW.md);
they are not the current API.

## Common questions

### Why is the default COBS body limit 255 instead of 256?

One byte represents 0..255. The default body is at most 253 useful bytes plus
a two-byte CRC. A 256-byte body needs H2. An explicit useful-payload limit
is never reduced: `Format<crc::Crc16Bitwise, 256>` carries all 256 payload
bytes with a two-byte length and a two-byte trailer.

### Does `Pool<8, 2>` mean byte sizes?

No. `wire::Pool<8, 2>` means eight RX owners and two TX owners. There is no
third parameter. Put the format on the endpoint:
`cobs::Endpoint<wire::Pool<8, 2>, cobs::Format<crc::Crc16Bitwise, 1024>>`.

### Why is there no COBS or UART TX queue?

Queueing policy belongs to the application: drop, retry, prioritize, or store
elsewhere. Both layers expose honest busy/exhaustion states without hiding
latency, memory, or ownership.

### Is UART half-transfer enabled?

No. STM32 HAL enables DMA HT interrupts on start, but this driver exposes no
half-transfer event and explicitly disables HT after every successful RX and
TX start. The code paths still mention HT because disabling it is required.

### Can `Packet` cross RTOS tasks?

Not under the current ownership contract. Its reference count is intentionally plain
and cheap. Keep copies/releases in one execution domain or design a separate
atomic ownership policy.

### What should happen after an RX overrun?

UART reports a gap in stream order. Forward it to `Endpoint::notify_gap()`.
COBS discards through the next delimiter and then resumes; do not treat the
first bytes after an unknown physical loss as a complete trusted frame.

## License

Project-owned code and documentation are licensed under the
[MIT License](../LICENSE), copyright © 2026
[shpegun60](https://github.com/shpegun60).

Git submodules and vendor-derived STM32 configuration files retain their own
licenses. See [third-party notices](../THIRD_PARTY_NOTICES.md).
