<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Modbus C++20 library

The current production layer is `modbus::rtu`: a deterministic Modbus RTU
framing endpoint with COBS-style ownership, heap or fixed-pool storage, CRC,
explicit metadata, and direct integration with the repository's STM32 DMA
UART.

`modbus::tcp` is architecturally reserved but is not shipped as a placeholder.
Its different streaming/MBAP framing contract is recorded in
[ARCHITECTURE.md](ARCHITECTURE.md).

## Quick start

```cpp
#include "modbus/rtu/Rtu.h"
#include "Uart.h"

using Serial = Uart<256, 4>;
using Link = modbus::rtu::Endpoint<modbus::rtu::Pool<8, 2>>;

static Serial uart;
static Link link;
```

Bind UART RX, ordered loss notification and borrowed TX:

```cpp
uart.setRxHandler(Serial::RxHandler{
    [](std::span<const uint8_t> candidate) noexcept {
        link.receive_adu(candidate);
    }});

uart.setRxGapHandler(Serial::GapHandler{
    []() noexcept {
        link.notify_gap();
    }});

const bool bound = link.bind(
    Link::Sender{tiny::bind<&Serial::send>(uart)},
    Link::BusyQuery{tiny::bind<&Serial::tx_busy>(uart)});
```

`receive_adu()` deliberately means one complete physical UART receive burst,
not arbitrary stream chunking. Use `Uart<256, N>` so a legal maximum RTU ADU
cannot be split merely by filling the DMA chunk.

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

These methods affect function data only. The user cannot write or reorder the
library-owned RTU envelope: address and function are one byte each, while the
configured 16-bit check value is generated and serialized low byte first.
`Message::size()` and `capacity()` count only function-data bytes.

## CRC calculation policy

`Endpoint` has one compile-time calculation policy:

```cpp
template<
    class StorageT = modbus::rtu::Heap,
    class CrcT = modbus::rtu::crc::Bitwise
>
class Endpoint;
```

The default remains the smallest portable choice:

```cpp
modbus::rtu::Endpoint<> link;
// Heap + table-free crc::Bitwise
```

The library ships exactly two calculators:

```cpp
using Memory = modbus::rtu::Pool<8, 2>;

using SmallFlash = modbus::rtu::Endpoint<
    Memory, modbus::rtu::crc::Bitwise>;

using FastCrc = modbus::rtu::Endpoint<
    Memory, modbus::rtu::crc::Table>;
```

Both built-ins implement CRC-16/MODBUS (`init=0xFFFF`, reflected polynomial
`0xA001`) and produce identical values. `Table` uses one constexpr 256-entry
`uint16_t` lookup table. Empty policy objects occupy no Endpoint RAM because
they are stored with `[[no_unique_address]]`; layout tests prove that both
built-ins have the exact same Endpoint size on x64 and Cortex-M.

A custom policy only has to satisfy this structural interface:

```cpp
template<class T>
concept Calculator = requires(
    T& calculator,
    std::span<const uint8_t> bytes)
{
    { calculator.calculate(bytes) }
        noexcept -> std::same_as<uint16_t>;
};
```

There is deliberately no semantic validation. The library does not recompute
CRC-16/MODBUS beside the policy, inspect its polynomial, or compare it with a
built-in. A custom policy may therefore implement a private 16-bit checksum,
including a wrapping sum:

```cpp
struct Sum16 {
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

using PrivateLink = modbus::rtu::Endpoint<Memory, Sum16>;
PrivateLink private_link;
```

That endpoint works normally when both peers use the same private checksum,
but its frames are not standard Modbus RTU. The names `crc_errors` and
`crc::verify` continue to mean “the received two-byte value does not match the
selected calculator” when a nonstandard policy is installed.

State is also allowed, so a policy can retain a peripheral handle:

```cpp
struct HardwareCrc {
    CRC_HandleTypeDef* handle;

    uint16_t calculate(
        std::span<const uint8_t> bytes) noexcept
    {
        return calculate_with_hardware(*handle, bytes);
    }
};

using HardwareLink = modbus::rtu::Endpoint<Memory, HardwareCrc>;
HardwareLink hardware_link{HardwareCrc{&hcrc}};
```

The same policy object is invoked for TX finalization and RX validation. There
is no virtual call, delegate, function pointer, or runtime algorithm branch in
`Endpoint`; a user policy itself may still choose an implementation by length:

```cpp
struct AdaptiveCrc {
    uint16_t calculate(
        std::span<const uint8_t> bytes) noexcept
    {
        if (bytes.size() < 32) {
            return modbus::rtu::crc::Bitwise{}.calculate(bytes);
        }
        return modbus::rtu::crc::Table{}.calculate(bytes);
    }
};
```

A stateful policy adds only its own state plus any unavoidable object-alignment
padding. RTU always excludes the received final two bytes from calculation,
compares the returned `uint16_t`, and stores/loads those bytes low first,
regardless of host endianness or selected algorithm.

Call both service methods from the same main-loop context:

```cpp
uart.proceed(HAL_GetTick());
link.poll();
```

## Read function data

Packet stores no mutable parser cursor. The application owns it:

```cpp
std::size_t offset = 0;
uint16_t start = 0;
uint16_t count = 0;

if (!modbus::read_be(packet.data(), offset, start) ||
    !modbus::read_be(packet.data(), offset, count)) {
    // malformed function data
}
```

`read_native`, `read_be`, `read_le` and `read_bytes` mirror the writer names.
They are bounds checked and leave both cursor and output unchanged on failure.
COBS exposes the identical calls as `cobs::read_*`. Both namespaces re-export
one implementation from `wire/Read.h`, so interface parity adds neither a
forwarding call nor duplicated endian logic.

## Storage choices

```cpp
// Dynamic exact allocations, convenient default.
modbus::rtu::Endpoint<> heap_link;

// Eight simultaneously owned RX packets and two TX messages/borrows.
using Fixed = modbus::rtu::Pool<8, 2>;
modbus::rtu::Endpoint<Fixed> fixed_link;
```

Pool counts are ownership quotas, not byte sizes. Every RTU pool block is
sized for the fixed protocol limits:

```text
PDU             1 function + 0..252 data = at most 253 bytes
RTU ADU         1 address + PDU + 2 CRC   = at most 256 bytes
minimum ADU     address + function + CRC  = 4 bytes
```

Retaining Packet copies consumes RX blocks. Holding unsent Messages or an
active UART borrow consumes TX blocks. Exhaustion returns an empty owner and
increments pool diagnostics; it never overwrites a live block.

Custom storage implementations satisfy `modbus::rtu::Storage` and the runtime
contract in [ARCHITECTURE.md](ARCHITECTURE.md#6-storage-extension-contract).
`MODBUS_POOL_CHECKS` is enabled by default so a built-in pool rejects foreign
or duplicate releases. Define it consistently in every translation unit if
you deliberately change that policy.

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

## RTU framing limitation

`receive_adu()` is correct for an already framed complete ADU. The current
direct UART adapter is a narrower burst framer: it treats one
`Uart<256,N>` ReceiveToIdle burst as one candidate and verifies that candidate
with CRC. It does not implement Modbus t1.5/t3.5 timing.

UART IDLE occurs after roughly one character, earlier than the Modbus t1.5
invalid-frame threshold. Therefore this adapter requires a peer that emits an
entire ADU as one uninterrupted UART burst and must not be advertised as
general strict-timing RTU interoperability. A future timed framer belongs
between UART events and `receive_adu()`; it must use timestamps tied to actual
RX events rather than delayed main-loop time.

## qmake

```qmake
include(path/to/modbus/rtu/rtu.pri)
```

Set `MODBUS_DELEGATE_DIR` before including the fragment only if
`tiny_delegate.hpp` is outside the repository's normal `libs/delegate` path.
The RTU implementation is header-only.

## Verification

```bash
PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH" \
    sh modbus/rtu/tests/run.sh

PATH="/c/Qt/6.10.1/mingw_64/bin:/c/Qt/Tools/mingw1310_64/bin:$PATH" \
    sh modbus/rtu/tests/qmake_consumer/run.sh

PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH" \
    sh modbus/rtu/tests/run_uart_integration.sh

sh modbus/rtu/tests/check_arm_layout.sh
sh modbus/rtu/tests/check_arm_crc_codegen.sh
sh wire/tests/check_arm_hotpath.sh
sh wire/tests/check_arm_codegen_matrix.sh
```

Real NUCLEO-H7S3L8 verification, including the independent Python CRC/ADU
oracle and the exact commands/results, is documented in
[rtu/tests/hardware/h7s/README.md](rtu/tests/hardware/h7s/README.md).

The suites cover public-header isolation, compile-fail API boundaries, known
and randomized CRC oracles, `Bitwise`/`Table` equality, a stateful fake-hardware
calculator, a deliberately nonstandard wrapping-sum TX/RX round trip, every
legal data length, 100,000 random ADU candidates, Heap/Pool conformance,
Packet lifetime, TX retry identity, exact 256-byte frames, qmake consumption,
and fake-HAL UART IDLE/TC/gap/DMA-borrow integration.

The final paranoid real-silicon matrix passed all suites at 115200 and 1M
baud. Its extended `-Os` 1M run echoed 7,319 exact ADUs / 632,449
function-data bytes in 15 seconds with zero unexpected RTU, UART, ownership,
or pool failures. Separate `-O2` and `-O3 + LTO` images passed the same 1M
functional/fault/pool path; see the hardware README and raw JSONL evidence.
A separate 3M probe observed one host write arrive as two CRC-invalid IDLE candidates.
That instrumentation did not measure the pause duration and therefore cannot
attribute the split solely to ST-Link VCP or distinguish peer discontinuity
from an over-eager IDLE boundary. The raw observation is retained without a
stronger claim.

The 2026-09-05 real-silicon A/B run tested both built-ins at 1M baud. Both
passed vectors, corruption/recovery, backpressure, pool exhaustion, 5-second
stress and 15-second stress with zero unexpected RTU/UART/storage failures.
Over 15 seconds, `Bitwise` used 1.197% measured integrated CPU and averaged
6,070 cycles in `receive_adu`; `Table` used 0.406% and averaged 1,128 cycles.
The linked Table image cost 500 additional text bytes (`23,120` vs `22,620`)
and changed neither data nor BSS. Raw evidence is in
[results_crc_policy_2026-09-05.jsonl](rtu/tests/hardware/h7s/results_crc_policy_2026-09-05.jsonl).

## Lifetime and concurrency

- Endpoint outlives all Packet and Message owners it created.
- Endpoint is not destroyed during an active transport borrow.
- Mutating methods are externally serialized.
- Packet reference counting is non-atomic and remains in one execution domain.
- UART error callbacks remain ISR-safe; RTU RX and gap handlers run from
  `Uart::proceed()` in thread context.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the complete invariant set and the
future `modbus::tcp` boundary.
