<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# One size convention: useful data bytes

The 2026-09-12 API update gives every protocol Format the same size unit:
**the bytes visible through `Packet::data()` and counted by Message::size()**.
Applications do not subtract headers or CRC to size ordinary payload buffers.
Maximum physical wire sizes and storage geometry are computed at compile time.
Selecting a wider CRC does not take bytes from an explicit data limit.

```cpp
using Memory = wire::Pool<8, 2>;
using Cobs = cobs::Endpoint<Memory, cobs::Format<crc::Crc16Table, 1024>>;
using Rtu = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<crc::Crc16Table, 1024>>;
using Tcp = modbus::tcp::Endpoint<Memory, modbus::tcp::Format<crc::NoCrc, 1024>>;
// Each exposes exactly 1024 data bytes. Physical frames and block sizes differ.
```

`make_message(..., hint)`, `reserve(n)`, `capacity()` and the Format limit all
use data bytes. `max_adu_size` / `max_frame_size` remain derived observations
for transports, not input parameters the application must calculate.
Standard-ADU constants are protocol facts, not buffer configuration.

## Standard defaults

| Endpoint | Default integrity | Useful data | Physical framing |
|---|---|---:|---|
| `modbus::rtu::Endpoint<>` | CRC-16/MODBUS Bitwise | 252 | 1 address + 1 function + data + 2 CRC; maximum 256 |
| `modbus::tcp::Endpoint<>` | NoCrc | 252 | 7 MBAP + 1 function + data; maximum 260 |
| `cobs::Endpoint<>` | CRC16 Bitwise | 253 | library length + payload + CRC, COBS encoded, delimiter |

RTU and TCP defaults retain their standard wire bytes. COBS itself standardizes
encoding, not a payload cap or integrity policy; 253 + CRC16 remains this
library's chosen default, not a COBS-standard requirement. Its explicit data
limits already had the desired meaning, so COBS production code is unchanged.

Larger actual Modbus frames or nonstandard checksum semantics remain explicit
private extensions. Nothing in this size-API update makes them standard Modbus.
TCP still has only MBAP framing, no user Framer; its default still has no CRC.

## What the library derives

For the same explicit `N`:

```text
RTU ADU maximum = N + 2 + Crc::wire_size
TCP ADU maximum = N + 8 + Crc::wire_size
COBS maximum    = encoding_bound(length_field + N + Crc::wire_size) + delimiter
```

For TCP, adding only MBAP's seven bytes would be one byte short: `function()`
is metadata too, outside `data()`. MBAP Length counts unit + function + data +
an explicitly selected private trailer; its width remains standard BE16.

RTU/TCP accept data limits from zero to `65533 - crc_size`. Their physical
maximums differ: RTU metadata permits 65535 total bytes, while TCP MBAP counts
only the bytes after its six-byte prefix, permitting 65541 total bytes.
Zero data still allocates the full mandatory envelope. Overflow, including
`SIZE_MAX` data or CRC sizes, fails at compile time before unsafe allocation.

`Geometry` still has only `rx_block_bytes`, `tx_block_bytes`, `alignment`.
RX is rounded for aligned slot arrays. Both endpoints add their private RX
ownership header. Memory receives physical byte requests and remains unaware
of CRC, Format, protocol or Packet internals. Equal-width Bitwise/Table share
Layout/Storage/Message/Packet; different CRC widths can change physical Pool
size while keeping the requested data capacity identical.

## Migrating earlier explicit Modbus limits

The second template argument previously meant the full ADU. It now means
data. This is a **source-level unit change**: numeric literals still compile
but no longer describe the same capacity. Default `Format<>` with each
protocol's default CRC preserves the standard wire and layout.

| Previous configuration | Same capacity with the new API |
|---|---|
| RTU `Format<Crc16, 256>` | `Format<Crc16, 252>` or simply `Format<>` |
| RTU `Format<Crc16, 1024>` | `Format<Crc16, 1020>` |
| TCP `Format<NoCrc, 260>` | `Format<NoCrc, 252>` or simply `Format<>` |
| TCP `Format<Crc16, 1024>` | `Format<Crc16, 1014>` |

For **new** code, choose the useful data limit directly. The subtraction above
is only a migration guide for an old physical budget, not a normal usage step.
Do not add ADU-budget aliases or a second competing public size-parameter API.

Explicit non-default CRC policies now default to 252 data bytes in both
Modbus formats. Their physical maximums adapt to CRC width. Earlier NoCrc RTU
had 254 data bytes inside a fixed 256 ADU; it now has 252 data / 254 ADU.
Specify 254 data explicitly if that previous private capacity is required.

The existing RTU function-data semantics are unchanged: a private function's
owned `length_prefixed` word is still part of that function's `data()` and
counts in Message::size(). This change does not strip/reinterpret application
PDU fields. Address/function/CRC and TCP MBAP are always outside `data()`.

## Verification and historical evidence

- `src/wire/tests/test_payload_limits.cpp`: 2,916 checks across **324**
  protocol/Memory/CRC/limit combinations: three protocols, Heap/Pool, all nine
  built-in CRC choices and 0/1/7/252/255/1024 data bytes. Equal input numbers
  must deliver equal useful sizes and reject one extra append. ASan/UBSan and
  O3/LTO are both run; MSVC x64/x86 includes this same suite.
- TCP: 75,016 core checks, 18,284 advanced checks and 1,620 dedicated limit
  checks; nine compile-fail cases. The latter add `SIZE_MAX`, one-over-Length,
  and CRC-width-aware maximums. MinGW, WSL sanitizer/O3/LTO and qmake consumers.
- RTU: existing suites, 11 updated compile-fail cases, all CRC widths at
  maximum/default/zero data, parser fuzz and the largest uint16_t ADU.
  Default ABI assertions remain pinned; wider trailers now legitimately grow
  Pool blocks instead of reducing payload. MinGW, WSL and MSVC x64/x86.
- ARM: TCP 72-object endian/access/optimization matrix, RTU layout and CRC
  guards, shared COBS/RTU scalar/protocol hot-path matrix. No new runtime size
  policy or endian detection is introduced.
- [Fresh H7S payload-limit receipt](../src/modbus/tcp/tests/hardware/h7s/README.md#payload-limit-api-repeat):
  six images, exactly 1024 TCP data bytes available with every tested policy,
  independent MBAP/CRC UART oracle, MCU-local RTU maximum/zero-data cases,
  real Heap/Pool exhaustion and original-flash restore/read-back.

Historical CRC/COBS/RTU performance records are not rewritten. Hardware
comparison harnesses that intentionally used a fixed physical ADU budget now
pass the corresponding data limit explicitly to preserve those experiments'
wire geometry. They do not define the application's default. Old receipts
identify their measured sources; see each record's provenance instead of
attributing earlier numbers to this new source tree.
