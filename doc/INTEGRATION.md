# Integration patterns: with and without adapters

This is the usage guide. It enumerates every supported way to put the
libraries in this repository together — the STM32 UART driver, the two
protocol endpoints (COBS and Modbus RTU), the RTU transport adapter and the
FreeRTOS wake glue — and says for each one what the application owns, what
it must call from where, and where the pattern is verified. The reference
documents stay where they are: `ARCHITECTURE.md` and `PROTOCOL.md` for COBS,
`src/modbus/ARCHITECTURE.md` and `src/modbus/README.md` for RTU, `STORAGE.md` for
memory, `UART_PARANOID_AUDIT.md` for the driver. This document only decides
which of them you need.

Every snippet below is a translation unit in `doc/examples/`, compiled and
run against the real headers by `sh doc/examples/build.sh` (the STM32 ones
on the host fake HAL, the FreeRTOS one on the recording FreeRTOS fake);
platform symbols such as `huart3` and `HAL_GetTick()` are the CubeMX ones
and come from `doc/examples/platform_fake.h` there. A snippet that stops
compiling fails that script.

## 1. What every pattern shares

Three layers, and the boundary between them never moves:

```
byte transport            protocol endpoint                 application
Uart / TCP / QSerialPort  cobs::Endpoint, modbus::rtu::Endpoint   Message / Packet
"bytes, in order,         "frames, integrity, packet          "what the bytes mean"
 with gaps announced"      lifetime, TX borrow"
```

The two endpoints are deliberately the same shape (`src/wire/tests/test_api_parity`):

| Call | Meaning | Context |
|---|---|---|
| `bind(Sender, BusyQuery)` / `unbind()` | the transport: one delegate that writes a frame, one that says whether the last frame is still borrowed | setup, thread |
| `consume(bytes)` | any cut of the byte stream (COBS always; RTU with a framing policy) | inside the transport's RX delivery, thread |
| `receive_adu(candidate)` | RTU without a framing policy: exactly one complete ADU | same |
| `notify_gap()` | bytes were lost between the previous and the next delivery | same |
| `pop_packet()` / `has_packet()` | the ready queue | thread |
| `make_message(...)` / `send(msg)` | build a frame in endpoint-owned memory, hand it to the transport | thread |
| `poll(now_ms)` | reclaim a transmitted frame once the transport is no longer busy | thread, every loop iteration |
| `stats()` / `storage()` | diagnostics | thread |

Three rules hold in every pattern:

- **The application owns time.** Nothing in the libraries reads a clock. The
  monotonic millisecond tick goes into `proceed(now)` / `poll(now)`, and every
  deadline is stamped with the tick the caller passed. Today both endpoints
  use the tick for nothing but the contract; the RTU stale-frame rule lives
  in the adapter (§2), which is where the geometry it needs lives too.
- **Delegates bind by reference, never by copy.** `tiny::bind<&T::method>(obj)`
  points at `obj`; `obj` must outlive the binding. Capture-less lambdas are
  the other accepted form. There is no heap and no `std::function` anywhere.
- **One execution context per endpoint.** The endpoint, its `Packet`s and its
  `Message`s are touched by one loop or one task; their reference counts are
  not atomic by design. The driver's handler setters are the only calls that
  are safe against a running ISR (they take the IRQ guard themselves).

Choosing a pattern:

| You have | Protocol | Pattern |
|---|---|---|
| STM32 with `src/uart/Uart.h`, bare-metal loop | RTU | §2, `UartAdapter` |
| STM32 with `src/uart/Uart.h`, FreeRTOS | RTU or COBS | §4, `FreeRtosWake` on top of §2 or §3 |
| STM32 with `src/uart/Uart.h` | COBS | §3, the driver wired directly — COBS needs no adapter |
| STM32 with another driver, or a stale-frame rule of your own | RTU | §5, the endpoint wired directly |
| desktop with Qt | either | §6, `adapters/qt/SerialAdapter.h`, plus `RtuClient.h` for a master |
| TCP, a test double, a radio | either | §7, any byte transport |

## 2. RTU on STM32 through `UartAdapter`

The adapter is the whole integration between the driver and an RTU endpoint:
RX and gap handlers, the endpoint's transport binding, the order of the
slow-path calls, and — for an endpoint with a framing policy — the rule that
decides when a frame that stopped arriving is dead. It lives in
`src/adapters/rtu/`, not in `src/modbus/`: it knows both the driver and the
endpoint, and neither of them knows it. It does not include the driver; it
reads the chunk geometry from the `Uart<ChunkSize, ChunkCount>` type and the
line rate from the driver's bound HAL handle.

```cpp
#define UART_ENGINE_IMPLEMENT          // in exactly one translation unit
#include "Uart.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/rtu/UartAdapter.h"

namespace framing = modbus::rtu::framing;
using Serial = Uart<256, 4>;
// A device that answers requests receives Direction::Request; a client
// receives Direction::Response. Drop the third parameter for the default
// burst endpoint (one complete ADU per IDLE-ended burst, no stale rule).
using Server = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
                                     framing::Standard<framing::Direction::Request>>;

__attribute__((section(".dma"))) static Serial serial;   // DMA-reachable RAM: the application's choice
static Server link;
static modbus::rtu::UartAdapter adapter{serial, link};   // takes no configuration: safe before main()

bool start() noexcept
{
    // init() first: bind() reads the line rate from the bound handle and is
    // refused, without side effects, while the driver is not initialized.
    return serial.init(&huart3) && adapter.bind();
}

void loop_step() noexcept
{
    adapter.proceed(HAL_GetTick());   // uart.proceed -> frame verdict -> link.poll
    while (auto request = link.pop_packet()) {
        auto reply = link.make_message(request.address(), request.function());
        if (!build_reply(reply, request)) {
            continue;
        }
        (void)link.send(reply);       // Sent moves ownership; Busy keeps the message for a retry
    }
}
```

What the adapter does, so the application does not:

- `bind()` is transactional: the endpoint's transport binding first, the
  driver's RX and gap handlers only when that succeeded; a false return
  (driver not initialized, handle without a rate, a transmission still
  active) changes nothing. `unbind()` mirrors it, and a bound adapter detaches
  itself in its destructor. The driver and the endpoint must outlive the
  adapter while it is bound, and one adapter serves one driver at a time.
- The line rate is re-read from the handle on every `proceed()`, so
  `Uart::setBaudRate()` is followed without a second call.
- With a framing policy, `proceed()` decides when an incomplete frame is
  dead: 5 ms of silence after a partial (IDLE-ended) chunk, one chunk's
  transfer time plus 5 ms after a full one (12-bit characters, the widest
  the driver accepts), and at the 5 ms it asks the driver's `rx_progress()`
  whether DMA is already receiving the remainder before it gives up. The
  snapshot is taken before the driver is drained and the verdict after, so a
  continuation already queued is never outrun by its own deadline. Detaching
  discards a frame in flight uncounted (`discard_incomplete()`).
- `deadline_in_ms(now)` says how long a scheduler may sleep: `no_deadline`
  while nothing is in flight, 0 when due (§4).
- A loop that must keep its own timing scopes around the driver composes the
  same steps itself, in this order and with one tick:
  `adapter.prepare(now); serial.proceed(now); adapter.finish(now); link.poll(now);`
  — the hardware harness does.

Verified by `src/adapters/tests/test_uart_integration.cpp` (the real driver on
the fake HAL through the adapter: lifecycle, baud changes, the stale rule at
9600 and 115200, the DMA-progress case, tick wrap) and on the H7S by
`src/modbus/rtu/tests/hardware/h7s/modbus_bench.cpp` with its records.

## 3. COBS on STM32: the driver wired directly

COBS needs no adapter. Frames end with a `0x00` delimiter, `consume()` takes
any cut of the stream, and there is no stale-frame rule to run: a sender
that dies mid-frame leaves a frame open, the next frame's bytes join it and
the delimiter that ends that next frame exposes the damage — the unfinished
frame and the first complete frame after it are lost together, counted, and
the stream is synchronized again at that same delimiter with nothing to
hunt for (`PROTOCOL.md` §8). The RX block the open frame held is returned
then. A gap the driver reports is handed on with `notify_gap()`.

```cpp
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "Cobs.h"

using Serial = Uart<256, 4>;
using Link = cobs::Endpoint<wire::Pool<8, 2>, cobs::Format<crc::Crc16Bitwise, 1024>>;

__attribute__((section(".dma"))) static Serial serial;
static Link link;

bool start() noexcept
{
    serial.setRxHandler(Serial::RxHandler{
        [](std::span<const uint8_t> bytes) noexcept { link.consume(bytes); }});
    serial.setRxGapHandler(Serial::GapHandler{
        []() noexcept { link.notify_gap(); }});
    // Either order works here: COBS needs nothing from the handle.
    return link.bind(Link::Sender{tiny::bind<&Serial::send>(serial)},
                     Link::BusyQuery{tiny::bind<&Serial::tx_busy>(serial)}) &&
           serial.init(&huart3);
}

void loop_step() noexcept
{
    const uint32_t now = HAL_GetTick();
    serial.proceed(now);   // the RX and gap handlers run here, in stream order
    link.poll(now);      // returns a transmitted block once the driver stops borrowing it
    while (auto packet = link.pop_packet()) {
        handle(packet.data());
    }
}
```

`Sender` and `BusyQuery` bind straight to the driver's `send()` and
`tx_busy()`: the frame stays in endpoint-owned memory until the driver's DMA
has finished with it, which is what `poll()` checks. The complete
application-shaped version with a pending-message policy is in the root
README ("Complete UART + COBS composition"); the exact silicon
implementation is `src/cobs/tests/hardware/h7s/cobs_bench.cpp`.

## 4. FreeRTOS on top of §2 or §3

`proceed()` is a thread-context call and the RX handler runs inside it, so a
sleeping task sees nothing until something wakes it. The driver's
`WakeHandler` is raised from the RX event, TX completion and error ISRs
after the driver's state is final; `src/adapters/freertos/FreeRtosWake.h` turns it into a
task notification. The driver knows no scheduler and the glue knows no
protocol.

```cpp
#include "adapters/freertos/FreeRtosWake.h"
#include <algorithm>

static uart::FreeRtosWake wake;                    // takes no task: safe at static-init time
static TaskHandle_t comm_task = nullptr;

void comm_task_body(void*)
{
    for (;;) {
        const uint32_t now = HAL_GetTick();
        // The adapter's deadline bounds the sleep: a frame whose remainder
        // never comes must be expired when it falls due, not when the next
        // unrelated frame wakes the task. For a COBS link (no adapter) the
        // fallback alone is the bound.
        (void)uart::FreeRtosWake::wait(std::min(50u, adapter.deadline_in_ms(now)));
        adapter.proceed(HAL_GetTick());
        while (auto request = link.pop_packet()) {
            serve(request);
        }
    }
}

bool start_comm() noexcept
{
    if (xTaskCreate(comm_task_body, "comm", 512, nullptr, 3, &comm_task) != pdPASS) {
        return false;
    }
    // After the handle exists. A null handle is refused and nothing is installed.
    return wake.attach(serial, comm_task);
}
```

Contract, in one place: the communication task is the only one touching
the driver, the endpoint and its packets; `wait()` is called by that task
only, and notification index 0 of that task belongs to the wake; the USART
and DMA interrupts must not be logically more urgent than the kernel's
syscall ceiling (on STM32 the HAL/CMSIS number given to
`HAL_NVIC_SetPriority()` is `>= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`;
never compare it against the shifted `configMAX_SYSCALL_INTERRUPT_PRIORITY`);
the kernel tick must be at least as fine as the deadlines used, or a coarse
tick turns the bounded wait into a busy service loop until the deadline
passes. Several interrupts before the task runs coalesce into one wake. Cost
with no handler installed: 4 cycles per interrupt (`UART_PARANOID_AUDIT.md`
§9.2).

Verified by `src/adapters/tests/test_freertos_wake.cpp` on the recording
FreeRTOS fake and the `WakeHandler` group of `src/uart/tests/host/test_uart.cpp`.

## 5. RTU on STM32 without the adapter

Wire the endpoint yourself when the driver is not `src/uart/Uart.h` (the
adapter reads its geometry through `UartTraits<Uart<ChunkSize, ChunkCount>>`
and calls `instance()` and `rx_progress()`), or when the stale-frame rule
must differ from the adapter's.

The default burst endpoint is the simple case. Its contract is that every
delivery is exactly one complete ADU, which the driver's IDLE-ended bursts
give as long as the chunk holds a whole ADU (`ChunkSize >= 256` for the
standard maximum):

```cpp
using Serial = Uart<256, 4>;
using Link = modbus::rtu::Endpoint<wire::Pool<8, 2>>;   // framing::None

__attribute__((section(".dma"))) static Serial serial;
static Link link;

bool start() noexcept
{
    serial.setRxHandler(Serial::RxHandler{
        [](std::span<const uint8_t> burst) noexcept { link.receive_adu(burst); }});
    serial.setRxGapHandler(Serial::GapHandler{
        []() noexcept { link.notify_gap(); }});
    return serial.init(&huart3) &&
           link.bind(Link::Sender{tiny::bind<&Serial::send>(serial)},
                     Link::BusyQuery{tiny::bind<&Serial::tx_busy>(serial)});
}

void loop_step() noexcept
{
    const uint32_t now = HAL_GetTick();
    serial.proceed(now);
    link.poll(now);
    while (auto request = link.pop_packet()) { serve(request); }
}
```

An endpoint with a framing policy takes `consume()` instead and needs a
stale-frame rule of yours, built from the same three calls the adapter
uses: `assembling()` (a frame is in flight), `expire_incomplete()` (drop it,
counted in `framing_stats().stale_frames`) and, when the transport is
detached, `discard_incomplete()` (drop it uncounted). The traps the adapter
had to be taught, so you do not learn them again:

- judge the frame only AFTER the driver has delivered what it already
  holds, never before, or a continuation queued at the deadline is thrown
  away;
- a partial chunk that ends in IDLE is not silence when DMA is already
  receiving the next chunk — ask the driver's `rx_progress()` before
  expiring;
- a full chunk means the line is busy: allow one chunk's transfer time plus
  a guard, with 12-bit characters, at the CURRENT line rate read from the
  handle, not a number captured at static-init time;
- deadlines are differences of the millisecond tick (`int32_t(now - deadline) >= 0`),
  so the tick may wrap;
- a gap disarms the deadline: `notify_gap()` already dropped the frame.

A minimal skeleton that observes all of them (`chunk_time_ms_at_current_baud()`
is yours: `kChunkSize * 12 * 1000 / baud`, rounded up, with the baud read
from `serial.instance()->Init.BaudRate`):

```cpp
namespace framing = modbus::rtu::framing;
using Framed = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
                                     framing::Standard<framing::Direction::Request>>;
static Framed link;
constexpr std::size_t kChunkSize = 256;   // the ChunkSize of Serial
static uint32_t g_now = 0, g_deadline = 0;
static bool g_armed = false;

void on_rx(std::span<const uint8_t> bytes) noexcept
{
    link.consume(bytes);
    g_armed = link.assembling();
    g_deadline = g_now + (bytes.size() < kChunkSize ? 5u : chunk_time_ms_at_current_baud() + 5u);
}

void loop_step() noexcept
{
    g_now = HAL_GetTick();
    const bool resumed = g_armed && int32_t(g_now - g_deadline) >= 0 && serial.rx_progress() != 0;
    serial.proceed(g_now);                                  // may run on_rx / on_gap
    if (g_armed && int32_t(g_now - g_deadline) >= 0) {
        if (resumed) { g_deadline = g_now + chunk_time_ms_at_current_baud() + 5u; }
        else         { g_armed = false; link.expire_incomplete(); }
    }
    link.poll(g_now);
}
```

If this is what you end up writing, use the adapter: it is this, tested.

## 6. Qt on the desktop: `QSerialPort`

`src/adapters/qt/` is the desktop counterpart of §2: `SerialAdapter` binds a
`QSerialPort` to either endpoint, and `RtuClient` is a Modbus master shaped
like Qt's own `QModbusRtuSerialClient` on top of it.

```cpp
#include "adapters/qt/RtuClient.h"

namespace framing = modbus::rtu::framing;
using Link = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>,
                                   framing::Standard<framing::Direction::Response>>;

QSerialPort port;
port.setPortName("COM6");
port.setBaudRate(9600);
port.open(QIODevice::ReadWrite);

Link link;
adapters::qt::RtuClient client{port, link};
client.update_timing_from_port();     // 3.5 character times below 19200 baud, as Qt computes it
client.bind();

const uint8_t body[] = {0x00, 0x6B, 0x00, 0x03};
client.send(0x11, 0x03, body, decltype(client)::Handler{[](const adapters::qt::Response& response) {
    if (response.state == adapters::qt::RequestState::Completed) { use(response.data); }
}});
```

`SerialAdapter` alone is enough for a server, for COBS, or for an
application that drives its own transactions: `readyRead` feeds `consume()`,
`bytesWritten` releases the transmitted block, a read or resource error
becomes `notify_gap()`, and a frame that stops arriving is expired 50 ms
after the last byte. An RTU endpoint here must carry a framing policy, since
a serial port delivers arbitrary cuts and not IDLE-ended bursts; the
adapter refuses the burst endpoint at compile time.

`RtuClient` adds what a master needs and what QModbus provides: one
transaction at a time with a queue behind it, a response timeout with
retries, matching by address and function with the exception bit masked off,
broadcasts to address 0 completed without an answer, an inter-frame delay
between transactions and a turnaround delay after a broadcast, and a clean
start before every attempt. Its defaults are Qt's: 1000 ms, three retries,
100 ms turnaround, 2 ms inter-frame at and above 19200 baud.

Where it deliberately differs from QModbus is written down in
`SerialAdapter.h`: Qt's RTU server drops a buffered fragment when the next
delivery arrives more than 3.5 character times after the previous one, which
on a desktop measures the operating system's scheduling rather than the
wire and can discard an intact frame that arrived in two deliveries; this
adapter uses a silence timer instead, which cannot. Qt reports a read error
to the application and keeps its buffer; this adapter treats it as a stream
discontinuity, because a lost byte inside a length-prefixed frame would
otherwise consume the frame behind it.

Build it with `include(src/adapters/qt/qt.pri)` next to `rtu.pri` or
`cobs.pri`; it adds `QT += serialport` and nothing else. Verified by
`sh src/adapters/qt/tests/run.sh` (82 checks on a `QIODevice` stand-in for
the port, both protocols, a real event loop, no COM port), and against
QtSerialBus itself on the H7S: `RtuClient` and `QModbusRtuSerialClient` run
the same 55-step script against the board's server and agree scenario for
scenario, and the board's client runs it against `QModbusRtuSerialServer`
(`src/adapters/qt/tests/hardware/h7s/README.md`).

## 7. Any other byte transport: TCP, tests, radios

The endpoints do not know what carries their bytes. A transport is any
object with a `send(std::span<const uint8_t>) -> bool` that writes one frame
and a `busy() -> bool` that says whether the LAST frame's memory is still in
use. `bind()` takes both; bytes are pushed into `consume()` wherever they
arrive, in any cut; `poll(now)` runs regularly and returns the transmitted
frame's block once `busy()` is false.

```cpp
#include "modbus/rtu/Rtu.h"
#include "Cobs.h"

// A transport that copies the frame (sockets, QSerialPort::write) may
// report busy() == false at once; one that borrows the memory (DMA) must
// report busy until it is done with it. The endpoint releases the frame in
// poll() either way.
struct Transport final {
    bool send(std::span<const uint8_t> frame) noexcept { return write_all(frame); }
    [[nodiscard]] bool busy() const noexcept { return false; }
};

namespace framing = modbus::rtu::framing;
// Off the STM32 driver there are no IDLE-ended bursts: TCP, the OS serial
// buffers and QSerialPort deliver arbitrary cuts, so the RTU framing policy
// is mandatory here. Direction is what THIS endpoint receives.
using RtuClient = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>,
                                        framing::Standard<framing::Direction::Response>>;
using CobsLink = cobs::Endpoint<>;   // wire::Heap, Crc16Bitwise, 253-byte payloads

Transport transport;
RtuClient client;
CobsLink cobs_link;

bool start() noexcept
{
    return client.bind(RtuClient::Sender{tiny::bind<&Transport::send>(transport)},
                       RtuClient::BusyQuery{tiny::bind<&Transport::busy>(transport)}) &&
           cobs_link.bind(CobsLink::Sender{tiny::bind<&Transport::send>(transport)},
                          CobsLink::BusyQuery{tiny::bind<&Transport::busy>(transport)});
}

void on_bytes(std::span<const uint8_t> bytes, uint32_t now_ms) noexcept
{
    client.consume(bytes);      // a fragment, several frames, or both
    client.poll(now_ms);
    while (auto response = client.pop_packet()) { handle(response); }
}

bool read_holding(uint8_t unit, uint16_t first, uint16_t count) noexcept
{
    auto request = client.make_message(unit, 0x03);
    return request.append_be(first) && request.append_be(count) &&
           client.send(request) == modbus::SendResult::Sent;
}
```

Two things the STM32 patterns get for free and this one must decide:

- **Frames that stop arriving.** With a framing policy the endpoint still
  exposes only `assembling()` and `expire_incomplete()`; when to call the
  latter is the transport's knowledge. Off the driver there is no chunk
  geometry and no DMA counter, only the transport's own delivery latency
  (an OS serial stack hands bytes over in bursts tens of milliseconds apart
  at low baud), so the silence limit must be chosen for that transport — a
  request/response client usually needs nothing beyond its response
  timeout, since it discards the incomplete response with the request.
- **Gaps.** A transport that can lose bytes without saying so (a radio, a
  UDP wrapper) cannot call `notify_gap()`; the CRC and the framing table
  then do the recovery, one frame at a time.

`wire::Heap` is the default memory and the right one here; `poll(now)` takes
any monotonic millisecond tick. Verified by `src/cobs/tests/qmake_consumer` and
`src/modbus/rtu/tests/qmake_consumer` (a loopback transport, both endpoints,
both built-in storages) and `src/wire/tests/test_protocol_storage` (a
user-written memory specification through both endpoints).

## 8. Choosing the parameters

**Memory.** `wire::Heap` (default) allocates per frame and is the desktop and
default path; `wire::Pool<Rx, Tx>` is `Rx` receive blocks and `Tx` transmit
blocks of the endpoint's exact geometry, statically owned, for deterministic
targets — size `Rx` for the frames in flight plus the packets the
application holds, `Tx` for the messages being built plus the one the
transport borrows; a user type with a nested `template<class Geometry> class For`
plugs in anything else (`STORAGE.md`, `src/wire/tests/test_protocol_storage`).
Changing memory changes neither the API nor the wire format.

**Format.** COBS: `cobs::Format<Crc = crc::Crc16Bitwise, RxMax = 255 - Crc::wire_size, TxMax = RxMax>`;
`Format<crc::NoCrc, 255>` is the byte-identical v1 wire format. RTU:
`modbus::rtu::Format<Crc = crc::Crc16Bitwise, MaxAdu = 256>`. The CRC policy
comes from `crc/` (`src/crc/README.md`): the Bitwise engines are the small ones,
the Table engines the fast ones, equal-width policies share every type
(`Layout`, `Storage`, `Message`, `Packet`); measured costs on the H7S are in
`PROTOCOL_COMPARISON.md`.

**Framer (RTU).** `framing::None` (default): one complete ADU per delivery.
`framing::Standard<Direction>`: frame ends found from the bytes, standard
functions and every exception response, Qt Serial Bus-compatible lengths.
A type derived from it adds private functions through `layout()`, with a
library-owned two-byte length prefix for the variable-length ones
(`src/modbus/README.md`, "RTU framing").

**Driver geometry.** `Uart<ChunkSize, ChunkCount>`: `ChunkSize >= 256` for
the burst RTU endpoint; the pool is `ChunkSize * ChunkCount` bytes of
buffering — about one millisecond at 10 Mbaud for `Uart<256, 4>`, which is
why the RTOS pattern wakes on the ISR rather than polling. The whole driver
object must sit in DMA-reachable RAM; the application places it.

## 9. Execution and lifetime rules

| Object | Lives at least as long as | Touched from |
|---|---|---|
| `Uart` | every handler bound to it, the adapter, the endpoint's transport binding | ISR (its own), one thread for `proceed()`/`send()`; setters guard themselves |
| endpoint | the adapter, every `Packet` and `Message` it handed out | one thread only |
| `UartAdapter` | — (detaches in its destructor; needs driver and endpoint alive) | one thread only |
| `FreeRtosWake` | the driver's use of it | ISR (`notify()`), the attached task (`wait()`) |
| `Packet` | as long as the application keeps the handle; the span is valid that long | the endpoint's thread |
| `Message` | until `send()` returns `Sent` (ownership moves) or the application drops it | the endpoint's thread |

Nothing of an endpoint is called from an ISR. The driver's RX handler runs
inside `proceed()`, so `consume()`/`receive_adu()` are thread calls even
though the driver invokes them. A `Packet` may be kept after `pop_packet()`
returns; it holds its RX block until the last handle is dropped, which is
what the pool's `Rx` count must cover.
