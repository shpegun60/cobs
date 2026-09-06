<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Modbus architecture

This document is the canonical ownership and namespace contract for the
Modbus library. It records the reviewed decisions that production code
follows, independently of the original working prompt.

## 1. Namespace boundary

There is intentionally no transport-ambiguous `modbus::Endpoint`.

```cpp
modbus::rtu::Endpoint<Memory, Format, Framer = framing::None>
modbus::tcp::Endpoint<Memory, Format> // future, not an empty placeholder
```

Only transport-independent application protocol facts live directly in
`modbus`:

- the standard Modbus maximum 253-byte PDU and 252-byte function-data limits
  used by the default RTU CRC16 format;
- `SendResult`, whose ownership meaning is transport-independent;
- stateless, bounds-checked PDU readers re-exported from the shared wire layer.

RTU and TCP may use common implementation primitives, but their framing,
storage geometry, packets and messages remain different types. Their memory specification is shared, but protocol-specific Packet and
Message types remain separate.

## 2. Shared public shape

The API deliberately follows the established COBS ownership vocabulary:

| Operation | COBS | Modbus RTU |
|---|---|---|
| dynamic storage | `wire::Heap` | `wire::Heap` |
| fixed storage | `wire::Pool<Rx, Tx>` | `wire::Pool<Rx, Tx>` |
| endpoint | `cobs::Endpoint<Memory, Format>` | `modbus::rtu::Endpoint<Memory, Format>` |
| receive owner | copyable `Packet` | copyable `Packet` |
| transmit owner | move-only `Message` | move-only `Message` |
| nullable/ownership API | `bool`, `reset`, `data`, `size` | same |
| transport setup | `bind` / `unbind` | `bind` / `unbind` |
| receive queue | `has_packet` / `pop_packet` | same |
| known byte loss | `notify_gap` | `notify_gap` |
| receive boundary | `consume(arbitrary_stream_chunk)` | `receive_adu(one_complete_candidate)`; `consume(chunk)` only with a framing policy (§8) |
| message factory | `make_message(hint)` | `make_message(address, function, hint)` |
| transmit | `send` returning `cobs::SendResult` | `send` returning `modbus::SendResult` with the same outcomes |
| scalar writing | `append_native` / `append_be` / `append_le` | same |
| raw bytes | `append_bytes` | `append_bytes` |
| scalar reading | `cobs::read_native/read_be/read_le` | `modbus::read_native/read_be/read_le` |
| byte reading | `cobs::read_bytes` | `modbus::read_bytes` |
| completion | `tx_active` / `poll(now_ms)` | `tx_active` / `poll(now_ms)` |
| observation | `stats` / `storage` | `stats` / `storage` |
| protocol metadata/views | none in the application payload | `address` / `function` / `pdu` / `adu` |

Modbus-specific metadata is explicit rather than serialized by the
application.

The differing receive boundary, message-factory metadata, and RTU Packet views
are protocol facts, not naming drift. Making those calls artificially identical
would hide whether an input span is an arbitrary stream chunk or one complete
CRC-bearing ADU.

The matching reader rows are one implementation, not merely similarly named
copies. `src/wire/Read.h` owns the bounds checks and strong failure guarantee;
`cobs` and `modbus` expose it with using-declarations. Both Packet types remain
immutable and cursor-free, while each application parser owns its offset.

## 3. RTU frame and logical payload

```text
RTU ADU = address | function | function data | policy-owned trailer
bytes       1          1          0..N          CrcT::wire_size
```

The physical ADU ceiling defaults to 256 bytes. `Format<Crc, MaxAdu>`
exposes `Layout<Crc::wire_size, MaxAdu>`, which owns protocol sizes, offsets,
minimum ADU length and useful data/PDU limits. MaxAdu 0/1 is rejected before
subtraction; the maximum is 65535 because RX metadata uses uint16_t.

```text
src/crc/Crc.h -> Format<Crc, MaxAdu> -> Layout<width, MaxAdu>
                                         |
src/wire/Storage.h <-- Endpoint derives Geometry and binds Memory::For<Geometry>
                                         |
                             Receiver / Message / Packet
```

Storage has no dependency on CRC or Format. The endpoint maps the protocol's
layout and private RX header into three physical geometry constants.

```text
max_data_size = MaxAdu - 1 address - 1 function - Crc::wire_size
```

A smaller ceiling is a local capacity restriction. Actual frames above 256 or
different checksum semantics are private exchanges; custom type identity does
not prove CRC-16/MODBUS compatibility.

The default two-byte CRC16 therefore retains the standard 252-byte function-
data limit. `NoCrc` exposes 254, CRC8 exposes 253, CRC32 exposes 250, and CRC64
exposes 246. Alternate widths define private RTU-like wire formats rather than
standard Modbus RTU, and both peers must select the same policy.

The public `Message::size()` and `Packet::size()` count function-data bytes
only. Address, function and CRC are protocol metadata/service bytes. They are
allocated in addition to the advertised data capacity.

```cpp
packet.address();  // RTU address metadata
packet.function(); // PDU function metadata
packet.data();     // function data only
packet.pdu();      // function + data
packet.adu();      // address + PDU + CRC
```

This definition gives `data()` and `size()` the same application-facing
meaning as COBS: the bytes supplied by the message builder, not framing bytes.
Policies with equal `wire_size` share one Layout (for equal MaxAdu), concrete Storage, Packet, Message, and
Receiver instantiation; choosing CRC16 Table instead of Bitwise does not
duplicate those ownership paths.

## 4. Packet ownership

`Packet` contains exactly one typed block pointer.

```text
Packet
  |
  +--> RxBlock
         refs                 uint32_t, non-atomic
         adu_size             uint16_t
         address/function     immutable metadata
         next_ready           intrusive queue link
         owner                exact Storage instance
         [complete ADU bytes]
```

The receiver validates CRC before allocation, copies the complete valid ADU
once into the block, stamps metadata, and places its existing reference on the
ready queue. `pop_packet()` moves that same reference into a `Packet` without
changing the count. Only copying or releasing a `Packet` changes `refs`.

Every view is a const span into the same block; `data()`, `pdu()` and `adu()`
perform no allocation and no copy. The endpoint must outlive all packets
created from it. Packet reference counting stays in one externally serialized
execution domain and is deliberately non-atomic.

## 5. Message ownership

`Message` exclusively owns one `wire::TxBlock {memory, granted}`. The block
descriptor is physical and CRC-agnostic: `granted` counts complete ADU
bytes. `Message::capacity()` exposes only
`Layout::data_capacity_for_adu(min(granted, Layout::max_adu_size))`, so application code still sees
function-data capacity with all envelope bytes hidden.

```text
Empty -> Building -> Finalized -> Sent
          |             |
          |             +-- Failed: retry byte-identical ADU
          +-- Busy: remains Building and caller-owned
```

- `append_native`, `append_be`, `append_le` and `append_bytes` are bounds checked;
- scalar and span overloads match the COBS Message vocabulary;
- ordered spans convert each element, never the complete array as one blob;
- a failed append leaves size, capacity and existing bytes unchanged;
- Heap messages grow geometrically; Pool messages receive the full slab;
- finalization calls the selected policy's `calculate()` and `store()` and is
  private to `Endpoint`;
- `Sent` moves the block into Endpoint;
- `Busy`, `Unbound`, `Failed` and `Invalid` do not steal caller ownership;
- `poll(now_ms)` releases the sent block only after the bound busy query is
  false; the tick is the same the UART driver's `proceed(now_ms)` takes and
  is not used by the endpoint today (§8: the endpoint holds no clock).

Standard Modbus multi-byte function fields use `append_be`. `append_native`
and `append_le` are explicit options for application/vendor-defined data; they
never affect the RTU envelope. C++20 `std::endian::native` and `if constexpr`
select the scalar path at compile time. On little-endian STM32, native/LE are
direct copies and BE uses the target's byte-reversal instruction, with no
runtime endian branch.

The library-owned fields do not use native serialization. Address and function
are one byte. The CRC policy owns trailer serialization through explicit
`store/load`; built-in policies use the protocol-independent `crc::Codec` and
never copy native object representation. COBS likewise owns its length prefix
and stores/loads it with explicit little-endian bytes. Payload serializers
cannot alter either envelope.

### CRC policy

RTU treats calculation and trailer representation as one static policy:

```cpp
template<class MemoryT = wire::Heap, class FormatT = modbus::rtu::Format<>>
class Endpoint;
```

Each Endpoint owns exactly one `[[no_unique_address]] CrcT m_crc`. The same
object is passed by reference to `Receiver::receive_adu()` and
`Message::finalize()`. Selection is therefore a template instantiation, not a
virtual call, delegate, function pointer, tag branch, or global singleton.
Endpoint obtains Crc and Layout from Format. Packet,
Message, and Receiver depend on Layout rather than on the algorithm type,
so policies of equal width reuse those types and their code.

The structural `crc::Policy` requirement is:

```cpp
typename CrcT::value_type;                  // equality comparable
constexpr std::size_t CrcT::wire_size;
CrcT::calculate(span) noexcept -> value_type;
CrcT::store(uint8_t*, value_type) noexcept -> void;
CrcT::load(const uint8_t*) noexcept -> value_type;
```

The independent `src/crc/Crc.h` module provides CRC8, CRC16, CRC32, and CRC64 in
Bitwise and Table forms plus `NoCrc`. Use `::crc::Crc16Bitwise` or
`::crc::Crc16Table` directly; no protocol-specific forwarding aliases remain. Built-in algorithms are empty
types and add zero Endpoint RAM on tested x64 and Cortex-M layouts.

Every lookup is a private static member of its exact Table specialization.
Including the header or merely naming Table types emits none. Calling a table
specialization emits one immutable 256-entry object of its result width in
read-only program memory; it never becomes an instance member or consumes RAM.

Custom policies are intentionally not semantically inspected. Endpoint does
not verify the initial value or polynomial and does not recalculate a built-in
CRC beside the policy. A custom `calculate()` may use hardware, assembler, an
adaptive length threshold, or even a non-Modbus wrapping sum. The latter is a
valid private protocol configuration as long as its peer agrees; it is not
standard Modbus RTU. A stateful policy may contain a peripheral handle and
increases Endpoint only by its state plus unavoidable alignment padding.

`crc::Codec<Integer, WireSize, WireOrder>` supplies reusable constexpr
store/load for custom integer policies. A custom policy can instead implement
all three operations itself, including a non-integer equality-comparable
result. RTU never assumes `uint16_t` or an endian order.

RX checks policy-derived minimum and physical maximum sizes, excludes exactly
`CrcT::wire_size` final bytes, calculates over the preceding bytes, loads the
trailer through the same object, and compares `value_type`. TX calculates over
address/function/data and asks the object to store its result. No fallback or
second calculation exists in either hot path. With a custom policy,
`crc_errors` records only a selected-policy mismatch.

`NoCrc::wire_size` is zero. Its calculation and codec inline away, RX performs
no integrity check, minimum ADU size becomes two, and the default CRC16's two
wire bytes become useful function-data capacity. This is an explicit private
format, not standard Modbus RTU.

## 6. Storage extension contract

The canonical contract is [shared raw-byte storage](../../doc/STORAGE.md).
A specification has `template<class Geometry> class For`. The instantiated
storage implements four noexcept methods:

```cpp
std::byte* acquire_rx(std::size_t bytes) noexcept;
void release_rx(std::byte*) noexcept;
wire::TxBlock acquire_tx(std::size_t bytes) noexcept;
void release_tx(wire::TxBlock) noexcept;
```

Geometry contains only an alignment-rounded maximum RX request, maximum TX
request and RX alignment. No RxBlock type, Format, CRC, minima or duplicate
payload limits are required from storage. The protocol constructs its header,
stamps the owner, copies the ADU and manages queue/refcount state.

TX grants may exceed the maximum request; capacity is separately clamped and
the original descriptor is returned unchanged. Undersized nonempty grants are
returned and refused without changing an existing Message. Every live region
is non-overlapping and returns to its issuing instance.

`wire::Heap` grants exact bytes; `wire::Pool<Rx, Tx>` grants fixed slabs
with independent quotas. Pool validation uses `WIRE_POOL_CHECKS`, enabled
even under NDEBUG. A custom strategy may add stronger alignment/placement or
external-arena state without learning any protocol metadata.

## 7. Transport contract

The sender/busy delegates are one transactional pair, identical to COBS:

```cpp
bool sender(std::span<const uint8_t> complete_adu) noexcept;
bool busy() noexcept;
```

Returning true means the transport borrowed the exact span and may continue
reading it until `busy()` becomes false. Returning false means no borrow was
taken. Binding, rebinding and unbinding are refused while Endpoint owns an
active TX block.

## 8. RTU receive boundary

`receive_adu(candidate)` does not consume arbitrary stream chunks. One call is
one physical receive burst produced by the supported adapter:

```text
Uart<256, N> + ReceiveToIdle DMA normal mode
short ADU      -> UART IDLE -> one candidate
256-byte ADU   -> DMA TC    -> one candidate
```

The receiver performs only:

1. policy-derived size `(2 + CrcT::wire_size)..MaxAdu` validation;
2. validation with the Endpoint's selected policy (`NoCrc` is a no-op);
3. RX storage acquisition;
4. one complete-ADU copy;
5. immutable Packet publication.

It never scans inside a rejected candidate and never infers length from a
function code.

This is a constrained STM32 burst-framing policy, not a strict software t1.5 /
t3.5 implementation. UART IDLE is roughly one character and therefore occurs
before the Modbus t1.5 invalid-frame threshold. A peer must transmit a complete
ADU continuously; even a protocol-legal sub-t1.5 pause can make this adapter
split it into partial candidates. CRC coincidences exist, and NoCrc validates
no integrity at all: the adapter must deliver one whole candidate. `receive_adu()` itself remains the API
for an already framed complete ADU.

The H7S silicon audit demonstrated both sides of this contract. Complete ADUs,
including the exact 256-byte maximum, passed at 115200 and 1M through the
ST-Link VCP. At 3M one 132-byte host write was observed as two candidates;
both failed CRC with no UART error. The instrumentation did not measure the
pause and cannot prove whether the peer violated t1.5 or the IDLE adapter made
an early boundary. That evidence is retained under `rtu/tests/hardware/h7s`
without attributing the cause solely to the VCP.

The default endpoint keeps framing outside itself: it neither grows a
timer-based framer nor infers boundaries from CRC or function-length tables,
and a transport adapter must call `receive_adu()` only when it has selected
one complete candidate. Length-based framing exists, but only as the explicit
third template parameter below.

### Optional framing policy

```cpp
using Server = modbus::rtu::Endpoint<Memory, Format,
    modbus::rtu::framing::Standard<framing::Direction::Request>>;
server.consume(chunk); // any cut of the stream; several ADUs per chunk
```

`Framer` defaults to `framing::None`, which is the endpoint described above,
byte for byte: same `sizeof`, same code paths, no function code interpreted.
Choosing a policy adds one thing to the wire contract — both peers agree on
how each function encodes the length of its data — and derives everything
else from it:

- **One declaration, both directions.** A `framing::Layout` per function
  (`fixed(n)`, `byte_count_at(offset, width)`, `length_prefixed(width)`,
  `unsupported()`) is read by the receiver to find the end of an ADU and by
  the message builder to reserve and fill a library-owned length prefix, and
  to refuse a message whose data disagrees with its layout
  (`SendResult::Invalid`, `framing_stats().tx_layout_rejected`). The two sides
  cannot drift, and an application can neither forget a length word nor
  miscount it.
- **`Standard<Direction>`** is the table of the application-protocol
  functions whose length follows from their own header (01–07, 0B, 0C, 0F,
  10, 11, 14–18 and every exception response `fn | 0x80`), taken from the
  worked examples of the specification and tested against them in both
  directions. `Direction` is the side this endpoint RECEIVES: 0x03 is four
  fixed bytes as a request and a byte count plus data as a response. 0x08
  Diagnostics frames as four bytes both ways, exactly as Qt Serial Bus does;
  its sub-function 0x00 Return Query Data with other than two data bytes
  therefore misframes and fails CRC (Qt's server rejects it too). 0x2B
  Encapsulated Interface Transport depends on bytes inside its data and is
  `unsupported()`. The table was checked differentially against Qt's
  `calculateDataSize` for every function code in both directions: the
  standard functions agree byte for byte; the only differences are 0x2B (Qt
  partially supports it) and exception codes in the request direction (Qt
  accepts a one-byte "exception request", the specification has none).
- **Private functions** extend the table by inheritance: override
  `layout(direction, function)` for your codes and forward the rest to
  `Base::layout`. `length_prefixed(2)` is the recommended shape for a
  variable-length private function: `[N: BE16][body]`, with `N` filled by the
  library; `packet.data()` on the peer is `[N][body]`, nothing is hidden.
- **Two-stage assembly, one copy.** Address, function and the bytes the
  layout needs are collected into a 16-byte local buffer; the data size is
  then known, `acquire_rx(header + adu)` is sized exactly, the remaining bytes
  are copied straight into the block and the CRC is checked in place. The
  RX block is therefore taken BEFORE the CRC check; when storage is exhausted
  the declared frame is skipped byte-exactly (`skipped_frames`) and the
  stream stays in step.
- **Recovery rule.** Length-based framing finds the end of a frame only when
  it knows its start. After an unsupported function, an oversize declaration
  or a CRC failure the receiver drops the remainder of the current chunk
  (`resyncs`) and treats the first byte of the next chunk as a frame start.
  This is the same precondition `receive_adu()` has always had — the
  transport delivers chunks eventually aligned to an inter-frame pause — and
  not an implementation of t1.5/t3.5. `notify_gap()` releases an in-flight
  frame.
- **Stale frames.** A frame whose sender died mid-frame would otherwise wait
  for its remaining bytes forever, holding an RX block and gluing itself to
  the next frame. The endpoint cannot decide when such a frame is dead: with
  DMA reception the software sees silence for a whole chunk's transfer time
  while the line is busy (a 256-byte chunk is 22 ms at 115200 and 294 ms at
  9600), so any limit needs the transport's chunk geometry and whether the
  line is still busy. The endpoint therefore holds no clock and exposes
  `assembling()` and `expire_incomplete()` (block returned,
  `framing_stats().stale_frames`); `UartAdapter` (§8a) owns the rule for the
  STM32 driver. `consume()` records no time.
- **`receive_adu()` stays available** and, under a policy, also refuses a
  candidate whose function has no layout (`unsupported_function`) or whose
  length disagrees with it (`length_mismatch`).
- **Measured.** On the H7S ST-Link bridge that splits frames from 3 Mbaud
  up, the framed endpoint echoed every single, split and glued frame at 1M,
  3M, 6M and 10M (12/12 each, zero CRC errors) and passed the full vector
  suite at every baud, where the default endpoint loses every split frame
  and most single frames above 3M. Record and method:
  [`rtu/tests/hardware/h7s/README.md`](rtu/tests/hardware/h7s/README.md#framing-policy-at-high-baud-2026-09-05).

`Packet`, `Message`, `Storage` and the wire bytes of every accepted frame are
identical with and without a policy; `Stats` is untouched and the
policy-only counters live in `FramingStats`. Client/server transaction logic
(request/response matching, timeouts, retries, register maps) remains a layer
above this one.

### 8a. UartAdapter: the integration object

`src/adapters/rtu/UartAdapter.h` — outside `src/modbus/`, because the endpoint must
not know that a concrete STM32 driver exists — is the whole glue between the STM32 driver
(`src/uart/Uart.h`) and an RTU endpoint, for either endpoint kind:

```cpp
static modbus::rtu::UartAdapter adapter{serial, link};   // no configuration: safe at static-init time
serial.init(&huart3);
adapter.bind();                  // transactional: endpoint transport first, then the driver's RX/gap handlers
adapter.proceed(HAL_GetTick());  // uart.proceed → expire an overdue frame → link.poll
```

It does not include the driver: `UartTraits<Uart<ChunkSize, ChunkCount>>`
reads the chunk geometry from the type, so the adapter compiles against the
host fake HAL exactly as against the silicon driver, and the driver stays
frozen. The line rate is read from the driver's bound HAL handle — the single
source of truth — at `bind()` and again on every `proceed()`, so a
`Uart::setBaudRate()` is followed without a second call; that is also why
`bind()` needs an initialized driver and why the constructor takes nothing (a
CubeMX handle carries no rate at static-init time). `bind()` and `unbind()`
are transactional — a false return leaves the driver's handlers and the
endpoint untouched — and a bound adapter detaches the driver's handlers in
its destructor; the driver and the endpoint must outlive it while bound.

The stale-frame rule lives here because only this layer has both facts. A
chunk of `ChunkSize` bytes ended by DMA transfer-complete (or by an idle
line exactly on the boundary, which the driver reports the same way and the
adapter treats the same, conservatively): the line is still busy, the next
chunk takes `ChunkSize` character times, so the deadline is that time plus a
5 ms guard, with a character taken as 12 bits — start, up to nine
data/parity bits and up to two stop bits, the widest frame the driver
accepts — so the deadline is never shorter than the wire. A shorter chunk
ended by IDLE while a frame is in flight: the line fell silent mid-frame,
and 5 ms of silence — counted from the IDLE event, itself about one
character after the last byte, so far beyond t1.5 at any usual Modbus baud —
is a dead frame — silence judged by the hardware, not by the absence of
events: when the 5 ms fall due the adapter asks the driver's `rx_progress()`
whether DMA has already taken bytes into the chunk it still owns, which is
the case for the whole transfer time of a resumed remainder that has not yet
ended in IDLE or filled the chunk (14 ms for 150 bytes at 115200, far longer
at 9600); then the frame is alive and the deadline becomes one chunk time
plus the guard. A bridge that splits a frame resumes within microseconds and
never reaches the 5 ms with zero progress. The full-chunk rule assumes a continuously
transmitting peer or bridge whose stalls inside a frame are small against a
chunk's transmission; strict RTU permits a pause below t1.5 between every
character, and a sender using that allowance on every byte could stretch a
chunk beyond the deadline — this adapter targets DMA peers and USB bridges
and is not a t1.5 timer. `on_rx()` stamps the deadline with the tick
`proceed()` was given; no time is read in the receive path. The progress
snapshot is taken before the driver is drained and the verdict after, so a
continuation already queued in the driver, or moved there while it drains,
is never outrun by its own deadline (a loop composing the steps itself keeps
`prepare → uart.proceed → finish → poll`); bytes that only begin to arrive
after the deadline instant are late by definition. A gap disarms the
deadline and reaches `notify_gap()`. `deadline_in_ms(now)` tells a scheduler
how long it may sleep, and a sleeping task must bound its wait by it.
`unbind()` and the destructor discard a frame in flight through
`discard_incomplete()`, uncounted: it is neither stale nor lost, and what
arrives after a later `bind()` could only glue onto it. With `framing::None`
the adapter routes bursts to `receive_adu()` and keeps no deadline.

Measured: on the fake HAL a 700-byte private ADU crosses three 256-byte
chunks at 9600 baud with 310 ms of software silence between them and
arrives whole, a sender that dies on a chunk boundary is expired after
325 ms, an orphan half after 5 ms, a 1 ms split is reassembled, a
continuation queued at the deadline completes, a bridge that resumes a split
frame into the next DMA chunk with no event yet keeps the frame alive at
115200 (the case the 5 ms rule alone got wrong: 7 failures before the
progress check), a baud change to 1M is followed on the next `proceed()`,
the tick wraps harmlessly, a refused `bind()` leaves the application's own
handler in place, and `unbind()` discards a frame in flight
(`test_uart_integration`); on the H7S the harness runs through the adapter
and the framed endpoint echoes 12/12 single, split, glued and orphan-then-
whole frames (`rtu/tests/hardware/h7s/README.md`).

## 9. UART gaps

The current UART never publishes the unreliable partial RX chunk that spans a
physical loss. Its ordered `GapHandler` is forwarded to `notify_gap()`, which
increments `stats.rx.stream_gaps`. There is no COBS-style byte resynchronizer:
the next complete physical burst is a new independently policy-validated
candidate.

## 10. Future TCP contract

TCP will preserve the same ownership verbs while exposing TCP metadata:

```cpp
packet.transaction_id();
packet.unit_id();
packet.function();
packet.data();
packet.pdu();
packet.adu();
```

The write side will correspondingly require transaction ID, unit ID and
function when creating a message. Its `Message` remains move-only and its
`Packet` remains a pointer-sized intrusive shared handle.

TCP framing cannot reuse RTU `receive_adu()`. `modbus::tcp::Endpoint::consume`
must accept arbitrary stream chunking, parse the seven-byte MBAP header,
validate protocol ID and length, and support partial or multiple ADUs per
call. A malformed length poisons that connection until an explicit reset or
disconnect; byte-scanning is not a sound TCP recovery policy.

TCP is added only as a complete tested layer. No empty `modbus::tcp` namespace
or placeholder types are shipped by the RTU phase.

## 11. Execution and lifetime rules

- Endpoint is non-copyable and non-movable.
- Endpoint outlives every Packet and Message created from it.
- Endpoint is not destroyed while `tx_active()` and the transport still
  borrows its block.
- Mutating calls are externally serialized.
- Packet copies/releases stay in one execution domain.
- Transport delegates are synchronous, `noexcept`, and do not re-enter the
  same endpoint's mutating methods.
- Stats are wrapping diagnostics, not atomic accounting.

## 12. Deliberate non-goals of the RTU framing layer

Of the default endpoint (`framing::None`):

- no function-code length tables;
- no length prefix;

These two exist only as the opt-in framing policy of §8 and never change the
default. Of every endpoint:

- no standard-function semantic parser;
- no CRC-based byte scanner;
- no t1.5/t3.5 software timers;
- no Client/Server transaction scheduler;
- no address filtering or broadcast response policy;
- no internal TX queue;
- no RTOS locking or atomic Packet;
- no UART ownership rewrite.

Client, Server and function helpers can be layered later over validated
`Packet::function()` / `Packet::data()` and the same Message builder.

## 13. Hot-path and size evidence

The receive path performs bounds checks and the selected policy before
allocation, then one copy of at most MaxAdu bytes into final Packet storage and
one O(1) intrusive
queue insertion. Packet copies and views touch only the block pointer and
reference count. The transmit path builds directly in its final contiguous
block, appends the selected trailer in place, and lends that same span to DMA;
no encoded-frame copy or virtual dispatch exists.

Historical pre-shared-storage measurements follow; current evidence is in
[SHARED_POLICIES_VALIDATION.md](../../doc/SHARED_POLICIES_VALIDATION.md).
The NUCLEO-H7S3L8 Pool-only images link no `new`, `delete`, `malloc`, `free`,
or `_sbrk` symbol. The original 2026-09-02 tableless baseline measured roughly
1.2% integrated CPU at 1M baud. The explicit 2026-09-05 A/B matrix then built,
flashed and identified both template policies over the same UART, pool,
traffic and `-Os` configuration:

| CRC policy / 15 s | Exact ADUs | Function-data bytes | Integrated CPU | `receive_adu` avg/max |
|---|---:|---:|---:|---:|
| `crc::Bitwise` | 7,337 | 634,317 | 1.192% | 6,011 / 16,997 cycles |
| `crc::Table` | 7,411 | 640,343 | 0.419% | 1,189 / 3,079 cycles |

Both passed vectors, four corruption/recovery cases, TX backpressure,
deterministic RX exhaustion, 5-second stress and 15-second stress with zero
unexpected RTU, UART, ownership or pool failures. On this target Table reduced
measured integrated CPU by 64.9% and average `receive_adu` cycles by 80.2%.
Those are end-to-end observations, not a cross-target timing guarantee.

The 1M linked `Bitwise` image is 22,728 bytes text; `Table` is 23,228 bytes.
Thus the 512-byte lookup replaces 12 bytes of bitwise code for a net 500-byte
text cost. Both images remain 12 bytes data and 6,832 bytes BSS. Object-code
guards across `-Os/-O2/-O3` prove that `Endpoint<>` emits no table symbol,
`crc::Table` emits exactly one private 512-byte read-only class table, and both
loops are inlined without helper calls. A wider standalone guard compiles all
CRC8/16/32/64 table policies at the same optimization levels: unused table
types emit zero lookup bytes, while each selected type emits exactly one
256-entry table of 256/512/1024/2048 bytes on both CPU byte orders. Default
and strict-alignment codec probes prove that every Bitwise/Table calculation is
helper-call-free, all 1/2/4/8-byte LE/BE wire paths are branch/call-free, and
`NoCrc` verification folds to constant true. The
default remains Bitwise, so table flash is paid only when that exact
implementation is called.

## 14. Normative references

- [Modbus Application Protocol Specification V1.1b3](https://www.modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf)
- [Modbus Serial Line Protocol and Implementation Guide V1.02](https://www.modbus.org/docs/Modbus_over_serial_line_V1_02.pdf)
- [Modbus Messaging on TCP/IP Implementation Guide V1.0b](https://www.modbus.org/docs/Modbus_Messaging_Implementation_Guide_V1_0b.pdf)
