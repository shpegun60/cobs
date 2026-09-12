<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Modbus C++20 library

`modbus::rtu` is a deterministic Modbus RTU
framing endpoint with COBS-style ownership, heap or fixed-pool storage, CRC,
explicit metadata, and direct integration with the repository's STM32 DMA
UART.

[`modbus::tcp`](tcp/README.md) is the header-only MBAP stream endpoint, with the
same storage/ownership/writer vocabulary and explicit transaction/unit metadata.
Its default is standard Modbus TCP without CRC; an explicit CRC policy or
oversized ADU is a private extension. It knows no transport and includes no
socket/TCP-IP stack. [H7S UART evidence](tcp/tests/hardware/h7s/README.md) checks
the core on silicon, not network interoperability. This page below covers RTU.

## Quick start

The compositions beyond this one — the endpoint without the adapter, Qt on
the desktop (`adapters/qt/RtuClient.h`, a master shaped like Qt's own
`QModbusRtuSerialClient`), a TCP transport, FreeRTOS on top — are enumerated
in [`doc/INTEGRATION.md`](../../doc/INTEGRATION.md).

```cpp
#include "modbus/rtu/Rtu.h"
#include "adapters/rtu/UartAdapter.h"
#include "Uart.h"

using Serial = Uart<256, 4>;
using Link = modbus::rtu::Endpoint<wire::Pool<8, 2>>;

static Serial serial;
static Link link;
static modbus::rtu::UartAdapter adapter{serial, link};   // takes no configuration: safe before main()
```

The adapter is the whole integration: UART RX and ordered loss notification
into the endpoint, the endpoint's borrowed TX onto the driver. It reads the
line rate from the driver's bound HAL handle, so the driver is initialized
first and the adapter bound after (a CubeMX `huart3` carries no rate until
`MX_USART3_UART_Init()` has run, which is why the adapter takes none at
construction):

```cpp
serial.init(&huart3);
adapter.bind();     // false if the driver is not initialized or a transmission is still active; then nothing changed
```

Without the adapter the same wiring is three explicit bindings — RX to
`receive_adu()`, gap to `notify_gap()`, `Sender`/`BusyQuery` to the driver's
`send()`/`tx_busy()` — and, with a framing policy, the stale-frame deadline
below; the adapter exists so that none of it lives in application code.

`receive_adu()` deliberately means one complete physical UART receive burst,
not arbitrary stream chunking. For the default format use `Uart<256, N>` so filling a DMA chunk does not
split a legal maximum ADU. Other ceilings require a suitably sized transport
or a complete-candidate adapter; CRC cannot substitute for framing.

## Receive packets

```cpp
while (auto packet = link.pop_packet()) {
    const uint8_t address = packet.address();
    const uint8_t function = packet.function();

    process(address, function, packet.data());

    // Additional zero-copy immutable views:
    inspect_pdu(packet.pdu()); // function + data
    inspect_adu(packet.adu()); // address + PDU + CRC
}
```

`Packet` is a pointer-sized copyable shared handle. Copies refer to one
storage-owned RX block containing immutable metadata and the complete ADU.
The last handle returns that block to `Heap` or `Pool`.

`data()` and `size()` mean function data only, matching the COBS convention
that application payload excludes framing metadata.

## Build and send a request

```cpp
auto message = link.make_message(
    1u,    // RTU address
    0x03u, // function
    4u);   // optional function-data capacity hint

if (!message ||
    !message.append_be(uint16_t{0x0010u}) ||
    !message.append_be(uint16_t{2u})) {
    return;
}

switch (link.send(message)) {
case modbus::SendResult::Sent:
    // Endpoint owns the ADU until UART releases the borrow.
    break;
case modbus::SendResult::Busy:
    // Message stays Building and caller-owned; retry later.
    break;
case modbus::SendResult::Failed:
    // The finalized byte-identical ADU remains retryable.
    break;
case modbus::SendResult::Unbound:
case modbus::SendResult::Invalid:
    break;
}
```

Available append operations:

```cpp
message.append_native(value); // target representation and byte order
message.append_be(value);     // big-endian scalar or scalar span
message.append_le(value);     // little-endian scalar or scalar span
message.append_bytes(raw);    // already serialized bytes
message.reserve(required_data_capacity);
```

There are no width-specific `append_u8` or `append_be16` methods. A one-byte
value has no byte-order distinction, so `append_native(uint8_t)`,
`append_be(uint8_t)` and `append_le(uint8_t)` emit the same byte. Standard
multi-byte Modbus function fields use `append_be`; native and little-endian
forms remain explicit tools for private/vendor-defined function data.

Scalar and span overloads share the COBS Message vocabulary. Ordered spans
convert every element independently. Padded structs, `bool`, pointers and
volatile objects are compile-time errors. Explicit-endian values are limited
to conventional 1/2/4/8-byte integer/enum/byte scalars plus `float` and
`double`; peers using floating point must additionally agree on its binary
representation.

Endianness is selected with C++20 `std::endian::native` and `if constexpr`.
There is no runtime byte-order branch. Little- and big-endian ARM builds prove
that M7 native-order scalar access is direct while the opposite 16/32-bit
order uses `REV16`/`REV`. ARMv6-M and strict-alignment variants instead prove
inline `LDRB`/`STRB` sequences with no copy helper. The focused and full guards
are [`check_arm_hotpath.sh`](../wire/tests/check_arm_hotpath.sh) and
[`check_arm_codegen_matrix.sh`](../wire/tests/check_arm_codegen_matrix.sh).

These methods affect function data only. The user cannot write address,
function, or the selected integrity trailer through an append call. The CRC
policy owns the trailer value, byte count, and byte order; `Message::size()`
and `capacity()` continue to count function-data bytes only.

## CRC policy and compile-time RTU format

`Endpoint<Memory, Format>` shares the same configuration shape as COBS.
The default is Heap with standard CRC-16/MODBUS and a 256-byte physical ADU:

```cpp
modbus::rtu::Endpoint<> link;
using Memory = wire::Pool<8, 2>;
using SmallFlash = modbus::rtu::Endpoint<
    Memory, modbus::rtu::Format<crc::Crc16Bitwise>>;
using FastCrc = modbus::rtu::Endpoint<
    Memory, modbus::rtu::Format<crc::Crc16Table>>;
using Smaller = modbus::rtu::Endpoint<
    Memory, modbus::rtu::Format<crc::Crc16Bitwise, 64>>;
using Larger = modbus::rtu::Endpoint<
    Memory, modbus::rtu::Format<crc::Crc32Table, 1024>>;
```

The number is **useful function-data bytes**, not an ADU budget:
`Format<crc::Crc16Bitwise, 64>` stores 64 data bytes and adds address, function
and CRC automatically (68-byte ADU). It means the same size unit as COBS/TCP.
A smaller MaxData is a local receive/send capacity, not a different protocol.
An actual frame above 256 bytes, or non-Modbus checksum semantics, is a private
RTU-like exchange. MaxData ranges from zero to `65533 - Crc::wire_size`;
the library checks overflow before deriving the physical ADU/storage sizes.

The two built-in CRC16 policies implement CRC-16/MODBUS (`init=0xFFFF`, reflected polynomial
`0xA001`) and produce identical two-byte little-endian trailers. Every lookup
is a private static member of its exact table-policy class. Merely including
the header, naming a table type, or selecting any bitwise policy emits no
table; calling this CRC16 table implementation emits one immutable 512-byte
object in program memory. Empty policies occupy no Endpoint RAM because they
are stored with `[[no_unique_address]]`.

The general library also supplies bitwise and table implementations of CRC8,
CRC32, and CRC64, plus `NoCrc`. RTU derives all logical geometry from the
selected policy and MaxData at compile time. The default data limit stays
252 when an explicit CRC policy is selected; storage grows around it:

| Policy width | `Endpoint::crc_size` | function-data capacity | maximum ADU | minimum ADU |
|---:|---:|---:|---:|---:|
| `NoCrc` | 0 | 252 | 254 | 2 |
| CRC8 | 1 | 252 | 255 | 3 |
| CRC16 (default) | 2 | 252 | 256 | 4 |
| CRC32 | 4 | 252 | 258 | 6 |
| CRC64 | 8 | 252 | 262 | 10 |

```cpp
using Crc8Link = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<::crc::Crc8Bitwise>>;
using Crc32Link = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<::crc::Crc32Table>>;
using Crc64Link = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<::crc::Crc64Bitwise>>;
```

The relationship is represented once:

```cpp
using Format = modbus::rtu::Format<CrcT, MaxData>;
using Layout = typename Format::Layout;

static_assert(Format::max_data_size == MaxData);
static_assert(Format::max_adu_size == MaxData + 2 + CrcT::wire_size);
```

Policies with the same `wire_size` and MaxData share `Layout`, Geometry,
concrete Storage, Message and Packet types; the Format types intentionally differ. Changing Bitwise to Table therefore does not duplicate those
ownership paths. A different width intentionally produces a different wire
format and different owner types.

A custom policy satisfies this structural interface:

```cpp
struct Policy {
    using value_type = /* equality-comparable result */;
    static constexpr std::size_t wire_size = /* trailer bytes */;

    value_type calculate(std::span<const uint8_t>) noexcept;
    void store(uint8_t* destination, value_type) noexcept;
    value_type load(const uint8_t* source) noexcept;
};
```

`crc::Codec<Value, WireSize, WireOrder>` supplies endian-independent integer
`store/load`, so most custom policies implement only `calculate()`:

This is the intentional migration boundary from the older calculate-only
policy: custom code must now declare its wire representation instead of RTU
silently forcing every result into a two-byte little-endian slot.

```cpp
struct Sum16 : crc::Codec<uint16_t, 2, std::endian::little> {
    uint16_t calculate(
        std::span<const uint8_t> bytes) noexcept
    {
        uint16_t value = 0;
        for (const uint8_t byte : bytes) {
            value = static_cast<uint16_t>(value + byte);
        }
        return value;
    }
};

using PrivateLink = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<Sum16>>;
PrivateLink private_link;
```

There is deliberately no semantic validation. The library does not recompute
CRC-16/MODBUS beside the policy, inspect its polynomial, or compare it with a
built-in. The wrapping sum above is used as-is. It works when both peers select
the same private policy, but it is not standard Modbus RTU. `crc_errors` means
only that the received policy-owned trailer did not match the selected policy.

State is also allowed, including a wider peripheral result:

```cpp
struct HardwareCrc32
    : crc::Codec<uint32_t, 4, std::endian::little> {
    explicit HardwareCrc32(CRC_HandleTypeDef& peripheral) noexcept
        : handle(&peripheral) {}

    CRC_HandleTypeDef* handle;

    uint32_t calculate(
        std::span<const uint8_t> bytes) noexcept
    {
        return calculate_with_hardware(*handle, bytes);
    }
};

using HardwareLink = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<HardwareCrc32>>;
HardwareLink hardware_link{HardwareCrc32{hcrc}};
```

The exact injected object is invoked for both TX finalization and RX
validation. There is no virtual call, delegate, function pointer, global
calculator, or runtime policy branch in Endpoint. A policy may itself select
an implementation by length:

```cpp
struct AdaptiveCrc
    : crc::Codec<uint16_t, 2, std::endian::little> {
    uint16_t calculate(
        std::span<const uint8_t> bytes) noexcept
    {
        if (bytes.size() < 32) {
            return ::crc::Crc16Bitwise{}.calculate(bytes);
        }
        return ::crc::Crc16Table{}.calculate(bytes);
    }
};
```

A stateful policy adds only its own state plus unavoidable alignment padding.
RX excludes exactly `CrcT::wire_size` trailing bytes from calculation, calls
that policy's `load()`, and compares its `value_type`; TX calls `calculate()`
and that policy's `store()`. No fixed integer type, byte order, or byte count
remains in RTU.

To remove the trailer entirely:

```cpp
using UncheckedLink = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<crc::NoCrc>>;
static_assert(UncheckedLink::crc_size == 0);
static_assert(UncheckedLink::max_send_size == 254);
```

`NoCrc` performs no integrity validation: every physically bounded candidate
of at least address+function length is accepted. The two bytes recovered from
the default CRC16 slot become useful function data. This is intentionally not
standard Modbus RTU and both peers must select the same private format.

One service call from the main loop (or one communication task):

```cpp
adapter.proceed();   // platform clock, uart.proceed → stale-frame check → link.poll
while (auto packet = link.pop_packet()) { handle(packet); }
```

## Read function data

Packet stores no mutable parser cursor. The application owns it:

```cpp
std::size_t offset = 0;
uint16_t start = 0;
uint16_t count = 0;

if (!modbus::rtu::read_be(packet.data(), offset, start) ||
    !modbus::rtu::read_be(packet.data(), offset, count)) {
    // malformed function data
}
```

`read_native`, `read_be`, `read_le` and `read_bytes` mirror the writer names.
The `modbus::rtu`, parent `modbus`, `cobs` and neutral `wire` reader names
refer to the same functions. Likewise all protocol `SendResult` names refer
to one `wire::SendResult` type. See the shared
[end-user API contract](../../doc/API_PARITY.md), including the same
`UartAdapter` lifecycle and the protocol-independent `uart::FreeRtosWake`.
They are bounds checked and leave both cursor and output unchanged on failure.
Enum readers accept `enum class`, not unscoped enums; for the latter, read
an integer and validate it before conversion. This prevents constructing an
invalid unfixed-enum value from wire bytes and adds no runtime checks to
integer fields. Writer support for already-valid enum values is unchanged.
COBS exposes the identical calls as `cobs::read_*`. Both namespaces re-export
one implementation from `src/wire/Read.h`, so interface parity adds neither a
forwarding call nor duplicated endian logic.

## Storage choices

```cpp
// Dynamic exact allocations, convenient default.
modbus::rtu::Endpoint<> heap_link;

// Eight simultaneously owned RX packets and two TX messages/borrows.
using Fixed = wire::Pool<8, 2>;
modbus::rtu::Endpoint<Fixed> fixed_link;
```

Pool counts are ownership quotas, not byte sizes. The same `wire::Pool<8, 2>`
specification goes into COBS and RTU unchanged; each endpoint binds its own
`Memory::For<Geometry>`. Geometry is three compile-time constants for the
maximum physical RX/TX requests and minimum RX alignment. Storage never sees
Format, CRC, packet metadata, or useful-payload units.

RTU asks RX for exactly its private header plus the complete validated ADU.
Its largest TX request is Format::max_adu_size. A Pool grants its entire slab,
while Heap grants exact requested bytes. `wire::TxBlock{memory, granted}`
preserves the original physical descriptor even when a size class overgrants.
Message caps useful capacity at MaxData; the physical envelope is automatic.

Retaining Packet copies extends RX block lifetime. Unsent Messages and the
active transport borrow consume TX blocks. Exhaustion returns an empty owner;
it never overwrites a live block.

Custom memory implements the same four noexcept raw-byte operations for both
protocols. See the complete [shared storage contract](../../doc/STORAGE.md).
`WIRE_POOL_CHECKS` defaults to 1 even under NDEBUG; its opt-out must be
consistent across translation units.

## Diagnostics

```cpp
const modbus::rtu::Stats stats = link.stats();

// stats.rx.candidates
// stats.rx.frames_received
// stats.rx.crc_errors
// stats.rx.too_short
// stats.rx.oversize
// stats.rx.allocation_failure
// stats.rx.stream_gaps

// stats.tx.frames_sent
// stats.tx.send_refused_busy
// stats.tx.send_failed
```

For Pool storage:

```cpp
link.storage().rx_available();
link.storage().tx_available();
link.storage().rx_stats();
link.storage().tx_stats();
```

## RTU framing: burst candidates by default, a framing policy on request

`receive_adu()` is correct for an already framed complete ADU. The direct
UART adapter is a burst framer: it treats one `Uart<256,N>` ReceiveToIdle
burst as one candidate and verifies that candidate with CRC. It does not
implement Modbus t1.5/t3.5 timing. UART IDLE occurs after roughly one
character, earlier than the Modbus t1.5 invalid-frame threshold, so a peer
must emit an entire ADU as one uninterrupted burst; on the H7S at 3 Mbaud
and above the ST-Link bridge splits long frames and the default endpoint
loses them.

The optional third template parameter finds frame ends from the bytes
themselves instead:

```cpp
#include "modbus/rtu/Rtu.h"
namespace framing = modbus::rtu::framing;

// A device that answers requests: it RECEIVES requests, sends responses.
using Server = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
                                     framing::Standard<framing::Direction::Request>>;
Server server;

// The adapter feeds any cut of the stream to consume() — several ADUs per
// chunk, a frame across chunks — and expires a frame whose sender died
// mid-frame (adapters/rtu/UartAdapter.h explains the rule and why it needs the chunk
// geometry and the baud).
modbus::rtu::UartAdapter adapter{serial, server};
void loop_step() noexcept { adapter.proceed(); }

// The builder knows the same table: a response to 0x03 is a byte count plus
// data, and a count that disagrees with the data is refused before the wire.
auto reply = server.make_message(0x11, 0x03);
reply.append_be<uint8_t>(4);
reply.append_be<uint16_t>(0x022B);
reply.append_be<uint16_t>(0x0064);
server.send(reply);
```

`framing::Standard<Direction>` covers the standard functions whose length
follows from their own header (01–07, 0B, 0C, 0F, 10, 11, 14–18 and every
exception response, and 0x08 Diagnostics as four bytes, like Qt Serial Bus);
0x2B Encapsulated Interface Transport depends on bytes inside its data and
is refused (`framing_stats().unsupported_function`). `Direction` is what this endpoint
receives — a client uses `Direction::Response`.

Private functions extend the table; a variable-length one should carry its
length in a two-byte prefix that the library owns:

```cpp
struct MyFramer : framing::Standard<framing::Direction::Request> {
    using Base = framing::Standard<framing::Direction::Request>;
    static constexpr framing::Layout layout(framing::Direction d, uint8_t fn) noexcept
    {
        if (fn == 0x41) return framing::Layout::length_prefixed(2); // [N: BE16][body]
        return Base::layout(d, fn);
    }
};
using Device = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>, MyFramer>;

auto frame = device.make_message(0x11, 0x41); // size() == 2: the prefix is reserved
frame.append_bytes(body);                      // the application appends only the body
device.send(frame);                            // N = body.size() is written before the CRC
```

The peer's `packet.data()` is `[N][body]`; nothing is hidden. Forgetting or
miscounting `N` is impossible because the application never writes it.
With `length_prefixed(1)`, the body is still limited to 255 bytes even in a
larger `Format`: `send()` returns `Invalid` if it cannot represent the count,
keeps the message writable, and never calls the CRC or transport. Use the
two-byte prefix for larger bodies. `Layout::store_count()` itself returns
`bool` and does not write on failure. Create layouts through the factories;
invalid offsets and widths return `unsupported()`.

What the policy does NOT do: it does not implement t1.5/t3.5 timing, and it
cannot find a frame start on its own. After an unsupported function, an
oversize declaration or a CRC failure it drops the remainder of the current
chunk (`framing_stats().resyncs`) and tries a fresh start on the next chunk.
A full DMA chunk need not start at a frame boundary: recovery requires an
eventually frame-aligned transport chunk, and may lose additional frames
before that happens. This is not COBS delimiter-based resynchronization.
An RX allocation failure
skips exactly the declared frame and keeps the stream in step. A frame that
stops arriving is dropped by `expire_incomplete()` and counted in
`framing_stats().stale_frames`; `assembling()` tells whether a frame is in
flight. The endpoint holds no clock — when a frame is dead depends on the
transport's chunk geometry and on whether the line is still busy, so the
`UartAdapter` decides: 5 ms of silence after a partial (IDLE-ended) chunk,
one chunk's transfer time (12-bit characters, the widest the driver accepts,
at the rate read live from the HAL handle) plus 5 ms after a full one. The
silence is the hardware's word, not the absence of events: at the 5 ms the
adapter asks the driver's `rx_progress()` whether DMA is already taking the
remainder into the next chunk, and if so the frame lives on for one chunk
time. Further extensions require new progress in that same unpublished
chunk, not merely the old non-zero counter. An empty framed `on_rx({})`
does not change the deadline. The full-chunk rule assumes a continuously transmitting peer or
bridge; a strict RTU sender that paused below t1.5 after every byte could
stretch a chunk beyond it. A task that sleeps between `proceed()` calls
bounds its sleep by `adapter.deadline_in_ms(now)`. With
`framing::None` (the default) nothing described in this section is compiled
in, and the endpoint is the one documented everywhere else in this file.

Measured on the NUCLEO-H7S3L8 through the ST-Link bridge, which splits
frames from 3 Mbaud up: the framed endpoint echoed every single, split and
glued frame at 1M, 3M, 6M and 10M and passed the full vector suite at each,
where the default endpoint loses every split frame and, above 3M, most
single ones. See
[the record](rtu/tests/hardware/h7s/README.md#framing-policy-at-high-baud-2026-09-05).

## qmake

```qmake
include(path/to/modbus/rtu/rtu.pri)
```

Set `MODBUS_DELEGATE_DIR` before including the fragment only if
`tiny_delegate.hpp` is outside the repository's normal `libs/delegate` path.
The RTU implementation is header-only. Its fragment includes
[`src/crc/crc.pri`](../crc/crc.pri) and [`src/wire/wire.pri`](../wire/wire.pri) automatically; standalone CRC consumers may
include that fragment directly.

## Verification

Current shared-policy migration evidence is recorded in
[SHARED_POLICIES_VALIDATION.md](../../doc/SHARED_POLICIES_VALIDATION.md).
The older measurements below remain labeled as historical comparison data.

```bash
sh src/crc/tests/run.sh

PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH" \
    sh src/modbus/rtu/tests/run.sh

PATH="/c/Qt/6.10.1/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH" \
    sh src/modbus/rtu/tests/qmake_consumer/run.sh

PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH" \
    sh src/adapters/tests/run.sh

sh src/modbus/rtu/tests/check_arm_layout.sh
sh src/crc/tests/check_arm_codegen.sh
sh src/modbus/rtu/tests/check_arm_crc_codegen.sh
sh src/wire/tests/check_arm_hotpath.sh
sh src/wire/tests/check_arm_codegen_matrix.sh
```

Real NUCLEO-H7S3L8 verification, including the independent Python CRC/ADU
oracle and the exact commands/results, is documented in
[rtu/tests/hardware/h7s/README.md](rtu/tests/hardware/h7s/README.md).

The suites cover public-header isolation, compile-fail API boundaries, known
and randomized independent CRC8/16/32/64 oracles, all bitwise/table pairs,
explicit little/big-endian codecs, a stateful fake-hardware calculator,
custom three-byte and wrapping-sum policies, `NoCrc`, policy-derived capacity,
every legal default data length, 100,000 random ADU candidates, Heap/Pool
conformance, Packet lifetime, TX retry identity, exact 256-byte frames, qmake
consumption, and fake-HAL UART IDLE/TC/gap/DMA-borrow integration.

The ARM code-generation guard builds all four selected table widths at
`-Os/-O2/-O3`. It proves that merely naming every table type emits zero table
bytes, while calling a selected table policy emits exactly one private
read-only 256-entry object of the expected 256/512/1024/2048-byte size.

Historical pre-shared-storage evidence (not proof of the current tree): the
paranoid real-silicon matrix passed all suites at 115200 and 1M
baud. Its extended `-Os` 1M run echoed 7,319 exact ADUs / 632,449
function-data bytes in 15 seconds with zero unexpected RTU, UART, ownership,
or pool failures. Separate `-O2` and `-O3 + LTO` images passed the same 1M
functional/fault/pool path; see the hardware README and raw JSONL evidence.
A separate 3M probe observed one host write arrive as two CRC-invalid IDLE candidates.
That instrumentation did not measure the pause duration and therefore cannot
attribute the split solely to ST-Link VCP or distinguish peer discontinuity
from an over-eager IDLE boundary. The raw observation is retained without a
stronger claim.

The pre-shared-storage 2026-09-05 real-silicon A/B run repeated both built-ins from the
protocol-independent CRC tree at 115200 and 1M. Both passed vectors,
corruption/recovery, backpressure, pool exhaustion, 5-second stress and
15-second stress with zero unexpected RTU/UART/storage failures. Over 15
seconds, `Bitwise` used 1.192% measured integrated CPU and averaged 6,011
cycles in `receive_adu`; `Table` used 0.419% and averaged 1,189 cycles. The
linked Table image cost 500 additional text bytes (`23,228` vs `22,728`) and
changed neither data nor BSS. All 23 records passed, and the runner restored
the default Bitwise/115200 image. Raw evidence is in
[results_crc_library_2026-09-05.jsonl](rtu/tests/hardware/h7s/results_crc_library_2026-09-05.jsonl).

The newer [nine-policy hardware benchmark](rtu/tests/hardware/h7s/CRC_BENCHMARK.md)
extends this evidence to NoCrc and both methods at all four widths. It separates
live calculator cycles, instrumented UART/RTU CPU cost and static instructions
in each exact flashed ELF. Its precomputed 300-frame/s traffic is a different
workload from the historical CRC16-only A/B figures above.

## Lifetime and concurrency

- Endpoint outlives all Packet and Message owners it created.
- Endpoint is not destroyed during an active transport borrow.
- Mutating methods are externally serialized.
- Packet reference counting is non-atomic and remains in one execution domain.
- UART error callbacks remain ISR-safe; RTU RX and gap handlers run from
  `Uart::proceed()` in thread context.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the complete invariant set and the
separate [`modbus::tcp` boundary](tcp/README.md).
