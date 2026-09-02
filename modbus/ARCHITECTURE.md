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
modbus::rtu::Endpoint<Storage>
modbus::tcp::Endpoint<Storage> // future, not an empty placeholder
```

Only transport-independent application protocol facts live directly in
`modbus`:

- the maximum 253-byte PDU and 252-byte function-data limits;
- `SendResult`, whose ownership meaning is transport-independent;
- stateless, bounds-checked PDU readers.

RTU and TCP may use common implementation primitives, but their framing,
storage geometry, packets and messages remain different types. This prevents
an RTU pool or message from being passed accidentally to a TCP endpoint.

## 2. Shared public shape

The API deliberately follows the established COBS ownership vocabulary:

| Operation | COBS | Modbus RTU |
|---|---|---|
| dynamic storage | `cobs::Heap<>` | `modbus::rtu::Heap` |
| fixed storage | `cobs::Pool<Rx, Tx>` | `modbus::rtu::Pool<Rx, Tx>` |
| endpoint | `cobs::Endpoint<Storage>` | `modbus::rtu::Endpoint<Storage>` |
| receive owner | copyable `Packet` | copyable `Packet` |
| transmit owner | move-only `Message` | move-only `Message` |
| transport setup | `bind` / `unbind` | `bind` / `unbind` |
| transmit | `make_message` / `send` | `make_message` / `send` |
| scalar writing | `append_native` / `append_be` / `append_le` | same |
| raw bytes | `append_bytes` | `append_bytes` |
| completion | `tx_active` / `poll` | `tx_active` / `poll` |
| observation | `stats` / `storage` | `stats` / `storage` |

Modbus-specific metadata is explicit rather than serialized by the
application.

## 3. RTU frame and logical payload

```text
RTU ADU = address | function | function data | CRC low | CRC high
bytes       1          1          0..252          1         1
```

`address_size`, `function_size`, `crc_size`, `adu_prefix_size`,
`pdu_envelope_size`, and `adu_overhead` are one constexpr geometry set.
`crc_size` is derived from `crc::wire_size`; Packet views, Message offsets and
storage sizes do not repeat numeric `2/3/4` framing constants.

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

`Message` exclusively owns one `TxBlock {memory, capacity}`. Capacity is in
function-data bytes. The physical block additionally holds address, function
and CRC.

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
- finalization adds CRC low byte first and is private to `Endpoint`;
- `Sent` moves the block into Endpoint;
- `Busy`, `Unbound`, `Failed` and `Invalid` do not steal caller ownership;
- `poll()` releases the sent block only after the bound busy query is false.

Standard Modbus multi-byte function fields use `append_be`. `append_native`
and `append_le` are explicit options for application/vendor-defined data; they
never affect the RTU envelope. C++20 `std::endian::native` and `if constexpr`
select the scalar path at compile time. On little-endian STM32, native/LE are
direct copies and BE uses the target's byte-reversal instruction, with no
runtime endian branch.

The library-owned fields do not use native serialization. Address and
function are one byte. CRC calculation produces a numeric `uint16_t`, and the
single `crc::store/load` wire codec always writes/reads low byte then high byte
with shifts. COBS likewise owns its length prefix and stores/loads it with
explicit little-endian bytes. User-selected serializers are therefore unable
to alter either framing contract.

## 6. Storage extension contract

`modbus::rtu::Storage<T>` verifies the public type and exception surface. A
custom implementation must additionally preserve these runtime obligations:

- `T::RxBlock` is exactly `modbus::rtu::RxBlock<T>`;
- `max_adu_size` is 256 and `max_data_size` is 252;
- `acquire_rx(n)` accepts `n <= 256` and returns either null or one uniquely
  owned, properly aligned `RxBlock` followed by at least `n` writable bytes;
- `acquire_tx(n)` accepts `n <= 252` and returns either an empty `TxBlock` or
  a unique block whose reported capacity is in `[n, 252]` and whose physical
  allocation holds at least `capacity + 4` bytes;
- successful live allocations never overlap;
- each block is returned to the exact storage instance that created it;
- release of a valid live block is allocation-free and `noexcept`;
- the storage object remains at a stable address until every Packet, Message,
  and active transport borrow originating from it has ended.

The built-in `Heap` uses exact dynamic allocations. `Pool<Rx, Tx>` uses two
independent fixed pools and therefore performs no dynamic allocation.
`MODBUS_POOL_CHECKS` defaults to `1`, detecting foreign and duplicate releases;
if an integration disables it after measurement, the macro value must be
identical in every translation unit that instantiates the same pool types.

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

1. size `4..256` validation;
2. Modbus CRC-16 validation;
3. RX storage acquisition;
4. one complete-ADU copy;
5. immutable Packet publication.

It never scans inside a rejected candidate and never infers length from a
function code.

This is a constrained STM32 burst-framing policy, not a strict software t1.5 /
t3.5 implementation. UART IDLE is roughly one character and therefore occurs
before the Modbus t1.5 invalid-frame threshold. A peer must transmit a complete
ADU continuously; even a protocol-legal sub-t1.5 pause can make this adapter
split it into CRC-invalid candidates. `receive_adu()` itself remains the API
for an already framed complete ADU.

The H7S silicon audit demonstrated both sides of this contract. Complete ADUs,
including the exact 256-byte maximum, passed at 115200 and 1M through the
ST-Link VCP. At 3M one 132-byte host write was observed as two candidates;
both failed CRC with no UART error. The instrumentation did not measure the
pause and cannot prove whether the peer violated t1.5 or the IDLE adapter made
an early boundary. That evidence is retained under `rtu/tests/hardware/h7s`
without attributing the cause solely to the VCP.

A future strict `modbus::rtu::Framer` must consume byte/burst events carrying
timestamps from the physical RX event, enforce t1.5/t3.5, and only then call
`receive_adu()`. Main-loop arrival time is not an adequate substitute, and CRC
scanning or function-length tables remain rejected framing strategies.

## 9. UART gaps

The current UART never publishes the unreliable partial RX chunk that spans a
physical loss. Its ordered `GapHandler` is forwarded to `notify_gap()`, which
increments `stats.rx.stream_gaps`. There is no COBS-style byte resynchronizer:
the next complete physical burst is a new independently CRC-validated
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

- no function-code length tables;
- no standard-function semantic parser;
- no hidden length prefix;
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

The receive path performs bounds checks and CRC before allocation, then one
copy of at most 256 bytes into final Packet storage and one O(1) intrusive
queue insertion. Packet copies and views touch only the block pointer and
reference count. The transmit path builds directly in its final contiguous
block, appends CRC in place, and lends that same span to DMA; no encoded-frame
copy or virtual dispatch exists.

The fresh NUCLEO-H7S3L8 Pool-only image linked no `new`, `delete`, `malloc`,
`free`, or `_sbrk` symbol. At 600 MHz its complete sequential 1Mbaud path
(UART IRQ/DMA, tableless CRC validation, RX copy, Packet/Message ownership,
response CRC, DMA start and release) used 1.216% measured CPU over 7,376 exact
round trips in 15 seconds. The final 115200 image occupied 22,584 bytes text,
12 bytes data, and 6,832 bytes BSS.

The tableless CRC is therefore a measured v1 space/speed choice, not an
unmeasured assumption. A lookup-table variant should be introduced only with
target evidence and an explicit flash-cost/API decision; it must not silently
change the one canonical implementation across translation units.

## 14. Normative references

- [Modbus Application Protocol Specification V1.1b3](https://www.modbus.org/docs/Modbus_Application_Protocol_V1_1b3.pdf)
- [Modbus Serial Line Protocol and Implementation Guide V1.02](https://www.modbus.org/docs/Modbus_over_serial_line_V1_02.pdf)
- [Modbus Messaging on TCP/IP Implementation Guide V1.0b](https://www.modbus.org/docs/Modbus_Messaging_Implementation_Guide_V1_0b.pdf)
