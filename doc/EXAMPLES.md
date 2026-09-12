<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Examples: choose a task, run it, adapt it

<!-- toc -->

Contents

- [Build and verification](#build-and-verification)
- [Portable protocol recipes](#portable-protocol-recipes)
- [STM32 and FreeRTOS recipes](#stm32-and-freertos-recipes)
- [Qt client, server and socket recipes](#qt-client-server-and-socket-recipes)
- [RTU framing and private functions](#rtu-framing-and-private-functions)
- [Custom storage and CRC, with state injection](#custom-storage-and-crc-with-state-injection)
- [What to change and what to keep](#what-to-change-and-what-to-keep)

<!-- /toc -->

[Documentation](README.md) · [Почни звідси](START_HERE_UK.md) · [Qt](QT.md) · [FreeRTOS](FREERTOS.md)

Every source listed in the runnable catalog has a builder and an executable
check. Platform setup and fake devices are labelled, not hidden library
requirements. The prose guides contain the relevant code and explain the
public calls, so opening a library header is optional.

## Build and verification

From the repository root:

```sh
python -B doc/check_docs.py
python -B doc/test_docs.py
sh doc/examples/build.sh
sh doc/examples/qt/build.sh
# Optional Linux/WSL memory/UB instrumentation:
DOC_SANITIZE=1 sh doc/examples/build.sh
```

The first command checks links/anchors, contents and synchronized excerpts;
the second runs the checker's own negative controls.
`build.sh` builds/runs 19 configurations: portable core programs and
STM32/FreeRTOS compositions against host fakes. Output is in
`doc/examples/out/`, with a compile log and executable per configuration.
`qt/build.sh` builds/runs three real-Qt event-loop programs; its serial I/O is
fake during self-test, and TCP uses real localhost sockets.

Use GCC/C++20 for the portable runner; set `CXX` if needed. On Windows run it
from Git Bash with the selected MinGW compiler on PATH, or under WSL. Run
WSL and MinGW **sequentially**, since they share output paths. Qt needs a kit
with SerialPort and Network; [Qt build commands](QT.md#build-and-run) show
tool overrides and actual-port modes. No cookbook builder reflashes a board.

## Portable protocol recipes

| Task | Complete source | What the program checks |
|---|---|---|
| COBS, RTU and TCP with the same writer/reader lifecycle | [protocols.cpp](examples/protocols.cpp) | creation, BE/LE/byte fields, all three RX entries, send/poll |
| Native scalar array and deliberate host representation | [protocols.cpp](examples/protocols.cpp) | span writer and scalar reader loop |
| A read past the end | [protocols.cpp](examples/protocols.cpp) | cursor and output unchanged |
| Retain a received packet after the original handle dies | [protocols.cpp](examples/protocols.cpp) | immutable shared allocation lifetime |
| Busy, refused start and retry | [backpressure.cpp](examples/backpressure.cpp) | pending Message survives; Failed frame is immutable |
| TX pool exhaustion and recovery | [backpressure.cpp](examples/backpressure.cpp) | release held Message, allocate again |
| One custom storage in all three protocols | [policies.cpp](examples/policies.cpp) | four operations, geometry-bound Pool wrapper, injection |
| Stateful private checksum / peripheral-shaped handle | [policies.cpp](examples/policies.cpp) | one instance used by RX and TX, no semantic CRC enforcement |
| Adaptive Bitwise/Table selection | [policies.cpp](examples/policies.cpp) | both branches agree for the chosen standard model |
| Whole-candidate RTU versus stream RTU | [rtu_direct.cpp](examples/rtu_direct.cpp) | split-candidate limitation and explicit expiry |
| RTU client + server, exception, owned private prefix | [rtu_framing.cpp](examples/rtu_framing.cpp) | RX roles, response data, custom layout and unknown RX function |
| MBAP byte-by-byte assembly | [tcp_stream.cpp](examples/tcp_stream.cpp) | exact wire bytes and metadata |
| TCP RX OOM with retained Packet | [tcp_stream.cpp](examples/tcp_stream.cpp) | skip known tail and recover without a new stream |
| TCP invalid header and new-stream reset | [tcp_stream.cpp](examples/tcp_stream.cpp) | fail-closed, input ignored, explicit fresh-boundary reset |
| Existing application-shaped custom transport | [any_transport.cpp](examples/any_transport.cpp) | RTU request/response and polling without UART |

`Example.h` is a small bounded copying transport plus a fail-fast check
function. It is test/example support, not a new library wrapper. Its CHECK
macro stays active regardless of NDEBUG. RTU private sums and TCP CRC/large
ADUs are not described as standard Modbus configurations.

## STM32 and FreeRTOS recipes

| Configuration in build output | Complete source | Mode / guide |
|---|---|---|
| `cobs_adapter` | [cobs_adapter.cpp](examples/cobs_adapter.cpp) | UART → COBS adapter, polling, exact echo |
| `rtu_adapter` | [rtu_adapter.cpp](examples/rtu_adapter.cpp) | Request-framed RTU, two requests with TX Busy, retained reply |
| `rtu_adapter_split` | [rtu_adapter.cpp](examples/rtu_adapter.cpp) | same checks with `DOC_SPLIT=1`; [prepare/UART/finish/poll order](INTEGRATION.md#rtu-adapter-split-servicing-and-lifecycle) |
| `rtu_uart_direct` | [rtu_uart_direct.cpp](examples/rtu_uart_direct.cpp) | complete manual client, explicit request budget, Busy/gap/timeout/DMA-lifetime controls |
| `rtu_uart_direct_wake` | [rtu_uart_direct.cpp](examples/rtu_uart_direct.cpp) | same with `DOC_WAKE=1`; [task integration](FREERTOS.md#rtu-with-wake-but-without-uartadapter) |
| `cobs_direct` | [cobs_direct.cpp](examples/cobs_direct.cpp) | manual RX/gap/send/busy, no adapter |
| `cobs_manual_wake` | [cobs_direct.cpp](examples/cobs_direct.cpp) | same program with `DOC_WAKE=1`, no protocol adapter |
| `uart_wake` | [uart_wake.cpp](examples/uart_wake.cpp) | raw UART, borrowed RX, caller-owned TX, ISR notification |
| `freertos_wake` | [freertos_wake.cpp](examples/freertos_wake.cpp) | framed RTU task wake, null handle rejection, notification coalescing |
| `cobs_freertos` | [cobs_freertos.cpp](examples/cobs_freertos.cpp) | COBS with the same wait/proceed adapter spelling |
| `freertos_entry_cobs` | [freertos_entry.cpp](examples/freertos_entry.cpp) | static task entry, self-attach, COBS pending echo |
| `freertos_entry_rtu` | [freertos_entry.cpp](examples/freertos_entry.cpp) | same entry with `DOC_RTU=1`, FC03 handler |

The other seven configurations in the runner are the portable sources above.
All STM32 examples use the **real UART/protocol code** over a fake HAL.
The host FreeRTOS scaffold records notifications but does not schedule real
tasks. It is not a board throughput or timing measurement.

The host-only `host_entry.cpp` wrapper defines the fake HAL before the example
globals in one translation unit. That gives deterministic destruction order:
the UART can abort/release while the fake peripheral model is still alive.
Without this, the original separately linked fixtures could use an already
destroyed fake-HAL map at process exit. This is a test-fixture lifetime fix,
not a change to the UART/protocol implementation.

For firmware, copy the application part, not `platform_fake.h`. Supply your
configured handle and real HAL/FreeRTOS headers. The complete task entry uses
the real API outside `DOC_HOST`; select the protocol with `DOC_RTU`. Keep one
UART callback implementation, DMA-readable storage and one owner task. See
[the full FreeRTOS guide](FREERTOS.md) for every wake operation and shutdown.

## Qt client, server and socket recipes

| Executable / mode | Source | Behavior |
|---|---|---|
| `qt_cobs --self-test` | [serial.cpp](examples/qt/serial.cpp) | paired fake serial ports, split input, real event loop, COBS echo |
| `qt_cobs --port NAME BAUD` | same | one real-port COBS request and bounded wait |
| `qt_cobs --server NAME BAUD` | same | real-port COBS echo server |
| `qt_rtu --self-test` | same, `EXAMPLE_RTU=1` | framed client/server, FC03 data, split input |
| `qt_rtu --port NAME BAUD` | same | one queued RtuClient request |
| `qt_rtu --server NAME BAUD` | same | small unit-1 register demo |
| `qt_tcp --self-test` | [tcp.cpp](examples/qt/tcp.cpp) | real localhost TCP, metadata-preserving echo, invalid-MBAP disconnect |

The self-test server returns register values 100/200 for the RTU client.
The actual-port demo expects the same data, not any arbitrary Modbus device.
`LoopPort.h` is serial-test scaffolding; the `--port/--server` branch uses
QSerialPort. The TCP Connection class is a bounded sample transport, not a
production socket adapter shipped in `src/adapters`.

## RTU framing and private functions

An application's RX direction determines which standard layout to use:

<!-- example: examples/rtu_framing.cpp#rtu-roles -->
```cpp
using Server = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Request>>;
using Client = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Response>>;
// Direction describes RX. TX validates the opposite direction's layout.
example::Transport upstream, downstream;
Client client;
Server server;
CHECK(example::bind(client, upstream) && example::bind(server, downstream));
auto request = client.make_message(0x11u, 0x03u);
CHECK(request && request.append_be(uint16_t{0}) && request.append_be(uint16_t{2}));
CHECK(client.send(request) == wire::SendResult::Sent);
server.consume(upstream.frame().first(3u));
CHECK(!server.has_packet() && server.assembling());
server.consume(upstream.frame().subspan(3u));
auto packet = server.pop_packet();
CHECK(packet && packet.address() == 0x11u && packet.function() == 3u);
auto reply = server.make_message(packet.address(), packet.function());
CHECK(reply && reply.append_be(uint8_t{4})); // two registers = four bytes
CHECK(reply.append_be(uint16_t{100}) && reply.append_be(uint16_t{200}));
CHECK(server.send(reply) == wire::SendResult::Sent);
client.consume(downstream.frame());
auto response = client.pop_packet();
CHECK(response && response.data().size() == 5u);
client.poll(0u);
server.poll(0u);
```
<!-- /example -->

For a custom function, extend only its known layout and fall back to the
standard implementation. This does not change CRC or storage:

<!-- example: examples/rtu_framing.cpp#private-framer -->
```cpp
struct PrivateRequests : framing::Standard<framing::Direction::Request> {
    using Base = framing::Standard<framing::Direction::Request>;
    static constexpr framing::Layout layout(framing::Direction direction,
                                             uint8_t function) noexcept
    {
        if (function == 0x41u) { return framing::Layout::length_prefixed(2u); }
        return Base::layout(direction, function);
    }
};
```
<!-- /example -->

`length_prefixed(2)` owns two BE length bytes on TX. Do not append them twice.
They are part of RTU function data, so RX reads them explicitly before body:

<!-- example: examples/rtu_framing.cpp#private-prefix -->
```cpp
using PrivateServer = modbus::rtu::Endpoint<wire::Pool<4, 2>,
    modbus::rtu::Format<crc::Crc16Bitwise, 64>, PrivateRequests>;
PrivateServer private_link;
CHECK(example::bind(private_link, downstream));
auto private_message = private_link.make_message(1u, 0x41u);
CHECK(private_message && private_message.size() == 2u); // reserved BE16 length
CHECK(private_message.append_be(uint32_t{0x01020304}));
CHECK(private_link.send(private_message) == wire::SendResult::Sent);
private_link.consume(downstream.frame());
private_link.poll(0u);
auto private_packet = private_link.pop_packet();
std::size_t offset = 0;
uint16_t length = 0;
uint32_t value = 0;
CHECK(private_packet && wire::read_be(private_packet.data(), offset, length));
CHECK(wire::read_be(private_packet.data(), offset, value));
CHECK(length == 4u && value == 0x01020304u);
```
<!-- /example -->

This layout applies to both directions of function 0x41 in the example.
Known standard layouts validate TX sizes; unknown TX layouts still pass
through. Unknown RX functions are not assembled by this framer. The framer
uses data lengths, not CRC or inferred Request/Response role switching.

## Custom storage and CRC, with state injection

The geometry is supplied by the endpoint; the wrapper knows no packet fields:

<!-- example: examples/policies.cpp#custom-memory -->
```cpp
struct Counts { unsigned rx = 0, tx = 0; };
struct ObservedMemory {
    template<class Geometry>
    class For {
        // The implementation can instead be an arena, slabs or external RAM.
        // Neither the geometry nor these four operations know the protocol.
        wire::Pool<4, 2>::For<Geometry> pool_;
        Counts& counts_;
    public:
        explicit For(Counts& counts) noexcept : counts_(counts) {}
        std::byte* acquire_rx(std::size_t bytes) noexcept {
            ++counts_.rx;
            return pool_.acquire_rx(bytes);
        }
        void release_rx(std::byte* memory) noexcept { pool_.release_rx(memory); }
        wire::TxBlock acquire_tx(std::size_t bytes) noexcept {
            ++counts_.tx;
            return pool_.acquire_tx(bytes);
        }
        void release_tx(wire::TxBlock block) noexcept { pool_.release_tx(block); }
    };
};
```
<!-- /example -->

The built-in Pool inside the wrapper handles aligned slots, exact original
descriptors and independent RX/TX quotas. To replace it with an arena, keep
the four public operations and satisfy all [storage obligations](STORAGE.md).
The library does not convert an unaligned/custom undersized block into safe
memory simply because a concept accepted its signatures.

An adaptive standard CRC policy can switch computation methods while sharing
one wire codec:

<!-- example: examples/policies.cpp#adaptive-crc -->
```cpp
struct AdaptiveCrc : crc::Codec<uint16_t, 2, std::endian::little> {
    uint16_t calculate(std::span<const uint8_t> bytes) noexcept {
        return bytes.size() < 32u ? crc::Crc16Bitwise{}.calculate(bytes)
                                  : crc::Crc16Table{}.calculate(bytes);
    }
};
```
<!-- /example -->

The full program also injects a deliberately private stateful sum with a
four-byte codec into COBS/RTU/TCP. For actual STM32 peripheral CRC use the
[ready Crc16 adapter](../src/adapters/stm32/Crc16.h) and
[its usage and measured constraints](HEAP_AND_HARDWARE_CRC.md). Hardware access
must finish synchronously, handle byte-aligned input, and have exclusive
ownership during calculation; these are not properties a concept can prove.

## What to change and what to keep

Change the application payload/register map, unit/transaction values, pool
counts, useful-data limit and agreed CRC policy. Replace the port name or
UART handle with your existing platform object. Choose RX role for RTU.

Keep checked construction/append results, retained pending ownership on Busy,
explicit failure policy, one execution domain, TX completion service and
owner lifetimes. Do not copy fake task handles, host-only scaffolding or old
legacy UART classes into firmware. Do not turn a local passing example into
an unqualified claim about every board, baud rate or network stack.
