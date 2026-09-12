<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Modbus TCP: a transport-independent C++20 endpoint

Include `modbus/tcp/Tcp.h`. The core knows bytes, MBAP, storage and ownership;
it knows nothing about sockets, Ethernet, UART, HAL, Qt or an RTOS.
It is not a transaction manager or register server. A UART can carry the same
ADU bytes for testing without starting a TCP/IP stack; that does not make the
exchange Modbus RTU or prove network interoperability.

## Familiar API, TCP metadata

```cpp
#include "modbus/tcp/Tcp.h"

using Link = modbus::tcp::Endpoint<wire::Pool<8, 2>>;
Link link;                       // Heap instead: modbus::tcp::Endpoint<> link;

auto message = link.make_message(0x1234, 0x11, 0x03); // transaction, unit, function
if (!message ||
    !message.append_be(uint16_t{0}) ||      // first register
    !message.append_be(uint16_t{10})) {    // register count
    // Allocation/capacity failure: do not send incomplete application data.
}
// Once a transport is bound: auto result = link.send(message);
```

The optional fourth factory argument is a function-data capacity hint, not
initial size. `size()` starts at zero. The message grows on append/reserve,
bounded by the Format. MBAP and any selected trailer are entirely library-owned.
The exact standard ADU above is `12 34 00 00 00 06 11 03 00 00 00 0A`.
The [qmake consumer](tests/qmake_consumer/main.cpp) builds and checks this example.

All scalar and span overloads mirror COBS/RTU: `append_native`, `append_be`,
`append_le`, `append_bytes`. Scalar restrictions and bounds-checked readers
are implemented by the shared `wire` library, not reimplemented here.

```cpp
link.consume(received_bytes);    // zero, partial, one or several ADUs
while (auto packet = link.pop_packet()) {
    auto transaction = packet.transaction_id();
    auto unit = packet.unit_id();
    auto function = packet.function();
    auto data = packet.data();   // function data ONLY
    auto pdu = packet.pdu();     // function + data, no MBAP/trailer
    auto adu = packet.adu();     // complete MBAP + PDU + optional private trailer

    std::size_t offset = 0;
    uint16_t first = 0, count = 0;
    if (!modbus::tcp::read_be(data, offset, first) ||
        !modbus::tcp::read_be(data, offset, count)) {
        // Application-specific malformed function data.
    }
    auto retained = packet;     // same immutable allocation; cheap shared handle
}
```

`read_native/be/le/bytes` leave output and offset unchanged on failure.
Packet has no mutable parser cursor. `Packet::size()` is `data().size()`;
resetting or destroying the last copy returns its memory to its exact owner.
No function code, exception code or unit value is interpreted by the framer.
Application validation and transaction-ID matching remain above this core.

## MBAP is the only framer

```text
[Transaction BE16][Protocol BE16 = 0][Length BE16][Unit u8][Function u8][Data]
                                                <------ Length ---------->
```

MBAP is seven bytes; Length counts Unit + PDU. Total ADU is `6 + Length`.
The receiver gathers the first six bytes, validates Protocol and Length,
then allocates one final RX block and copies the counted tail into it.
There is no large intermediate frame buffer, no function-length table, no
Request/Response mode and no replaceable `Framer` template argument.
Custom functions need no additional payload-length word: MBAP already has it.

The default is `Format<crc::NoCrc, 252>`: up to 252 data bytes and an eight-byte
minimum ADU. Multibyte MBAP fields are always big-endian, independently of the
CPU and independently of how the application appends its data.

MBAP is length-delimited, **not self-synchronizing**. Lost/inserted bytes or a
corrupt plausible Length cannot be repaired by searching for a header.

## Format, CRC and shared storage

```cpp
using Standard = modbus::tcp::Endpoint<>;  // Heap + NoCrc, standard TCP ADU
using Small = modbus::tcp::Endpoint<wire::Pool<4, 2>,
    modbus::tcp::Format<crc::NoCrc, 64>>;   // 64 useful data bytes
using Private = modbus::tcp::Endpoint<wire::Pool<4, 2>,
    modbus::tcp::Format<crc::Crc16Table, 1024>>;
```

The spelling matches RTU: `Format<Crc, MaxData>`. MaxData counts only `data()`,
just like the payload limit in COBS. MBAP (7 bytes), function (1 byte) and the
optional CRC are added automatically; selecting a wider CRC does not reduce
the requested data capacity. For example, `Format<crc::Crc16Table, 1024>` holds
1024 data bytes and sizes its ADU/storage for 1034 bytes.

The valid data range is `0 .. 65533 - crc_size`. Length remains 16-bit, so the
derived complete ADU can be six bytes larger than 65535.
Frames exceeding 260 bytes or carrying a CRC trailer are **private extensions,
not standard Modbus TCP**. The core never silently switches format.

For a private CRC format:

- Length counts `unit + function + data + CRC` and is filled before calculation.
- Calculation covers **MBAP + function + data**, excluding only the trailer.
- Trailer width, algorithm and byte order come exclusively from `crc::Policy`.
- Data/PDU views exclude the trailer; ADU includes it.
- The same policy object handles RX and TX. Bitwise/Table add no per-object
  state; unused tables produce no symbols or memory. A custom policy can use
  hardware or a wrapping sum: only the structural/noexcept contract is checked.

`Format<>` keeps the standard 252-data/260-ADU default; an explicit CRC policy
keeps 252 data bytes but increases the physical ADU by its trailer width.
The [CRC guide](../../crc/README.md) describes all policies and custom codecs.
Stateful injection is identical to RTU:

```cpp
using Custom = modbus::tcp::Endpoint<MyMemory,
    modbus::tcp::Format<MyCrc, 1024>>;
Custom link{MyCrc{handle}, std::in_place, storage_arguments};
```

With default storage, use `Link{MyCrc{handle}}`; with only custom storage
construction, use `Link{std::in_place, storage_arguments}`. These are illustrative
user-defined types, not extra mandatory library interfaces.

Both ends must agree on the extension. Protocol ID stays zero; there is no
CRC/version negotiation. A NoCrc receiver may accept a CRC-bearing frame and
expose its trailer as application data. Never mix configurations expecting an
automatic incompatibility rejection. CRC also does not provide authentication.

`max_send_size` / `max_receive_size` count data; `max_frame_size` counts the ADU.
`Geometry` exposes exactly `rx_block_bytes`, `tx_block_bytes`, `alignment`.
RX slot bytes are rounded to alignment. `Memory::For<Geometry>` uses the
[same four byte-storage operations](../../../doc/STORAGE.md) as COBS/RTU.
Layout keys only trailer width and limit; equal-width Bitwise/Table share
Storage, Message and Packet types. Packet itself is pointer-sized.

TX overgrants are retained and returned unchanged; usable capacity is capped
by Format. Undersized grants are released and rejected. A failed growth leaves
the same message's size, capacity and existing data unchanged.

## Recovery and ownership

| Event | RX behavior |
|---|---|
| Partial header/body | bounded assembly; wait for more bytes |
| Valid complete ADU | publish one immutable Packet; continue in the same chunk |
| Allocation failure | count failure, skip exactly the declared tail, continue at the next ADU |
| Invalid Protocol, too-small Length, oversized ADU | fail closed before allocation |
| CRC mismatch in a private format | fail closed; Length itself may have been corrupt |
| `notify_gap()` | release incomplete RX, count the gap, fail closed |
| `reset_rx()` | release incomplete RX and start a caller-guaranteed new stream |

`rx_failed()` remains true and further input is ignored until reset.
`assembling()` includes a partial header/body and an OOM skip; it is false in
the failed state. There is no core timer: a peer that advertises a legal size
and stops can retain a bounded partial block until the adapter's timeout or
disconnect. The adapter must close/stop a failed stream and reset only at a
known new boundary, never at an arbitrary next receive callback.

Reset/gap do not erase completed queued packets, retained Packet copies,
statistics or active TX borrows. If an application must discard responses
from an old connection, drain its queue explicitly at connection teardown.
Use one endpoint/parser state per independent byte stream.

RX counters: `candidates` (complete six-byte prefixes), `frames_received`,
`invalid_protocol`, `invalid_length`, `oversize`, `crc_errors`,
`allocation_failure`, `skipped_frames` (fully consumed OOM frames), `stream_gaps`.
TX counters match RTU/COBS: `frames_sent`, `send_refused_busy`, `send_failed`.

## Bind any byte transport

`Sender` is `tiny::delegate<bool(std::span<const uint8_t>)>`;
`BusyQuery` is `tiny::delegate<bool()>`. Bind both with `bind(sender, busy)`.
No virtual transport base class is required. Delegates must not throw or
re-enter the endpoint. `bind/unbind` refuse an active endpoint-owned TX borrow;
a failed bind leaves the old binding intact.

- Sender accepts the **entire** borrowed span, or returns false having accepted
  none. It may keep the span only while BusyQuery remains true.
- `Sent` empties Message and transfers its block to the endpoint. It means
  acceptance, not delivery. Call `poll(now_ms)` to release once BusyQuery is false.
- `Busy`/`Unbound` retain a writable message. `Failed` retains a finalized,
  immutable message for a byte-identical retry. Empty/foreign messages are Invalid.
- A real socket adapter must handle partial writes internally, retaining the
  entire borrow until completion/abort. Returning false after writing a prefix
  violates this contract. No such socket adapter is included in this slice.
- Stop the transport before destroying Endpoint. Endpoint/storage outlive
  all issued Packet/Message owners. Externally serialize every operation and
  non-atomic reference-count update. RX parsing runs in application context.

For UART DMA tests, Pool TX and Heap backing memory must be DMA-readable;
the protocol itself imposes no HAL/cache constraints. The harness uses AXI
SRAM and the unchanged UART driver's cache maintenance. Applications remain
free to place their protocol storage according to their actual transport.

## Build and verification

Header-only C++20: `-I src -I libs/delegate`, or include `tcp.pri` from qmake.

```sh
sh src/modbus/tcp/tests/run.sh
sh src/modbus/tcp/tests/check_arm.sh
sh src/modbus/tcp/tests/qmake_consumer/run.sh
```

See [implementation contract](../../../doc/MODBUS_TCP_PLAN.md),
[API parity](../../../doc/API_PARITY.md) and [live H7S evidence](tests/hardware/h7s/README.md).
The [payload-limit migration](../../../doc/PAYLOAD_LIMITS.md) fixes the same
size-parameter meaning across COBS, RTU and TCP. Host tests include
ASan/UBSan under WSL, O3/LTO, MinGW, MSVC x64/x86, independent
headers, rejected contracts, all CRC widths, random streams, real/custom storage
faults and maximum Length. The MCU suite uses an independent MBAP/CRC wire oracle.
Live UART evidence does **not** claim sockets, TCP retransmission, Ethernet,
multi-connection behavior, transaction scheduling or network interoperability.

Normative default: [Modbus Messaging on TCP/IP Implementation Guide V1.0b, section 3.1](https://www.modbus.org/file/secure/messagingimplementationguide.pdf).
