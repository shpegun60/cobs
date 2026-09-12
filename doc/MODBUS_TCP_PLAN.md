<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# Modbus TCP: transport-independent implementation contract

This slice adds the Modbus TCP ADU codec and ownership layer, not a TCP/IP
stack, socket client, register server or Ethernet driver. The initial TCP-only
slice left COBS/RTU/UART unchanged; the subsequent [data-limit migration](PAYLOAD_LIMITS.md)
and [cross-stack audit](PARANOID_AUDIT_2026-09-12.md) record their separate
RTU geometry and UART/Qt fixes. A UART may carry exact TCP ADU bytes for MCU verification; that
does not become a Modbus RTU frame or prove network interoperability.

## Locked public decisions

- `modbus::tcp::Endpoint<Memory = wire::Heap, Format = tcp::Format<>>`.
- `wire::Heap`, `wire::Pool<Rx, Tx>` and the same custom `Memory::For<Geometry>`
  specification are accepted, with no TCP-specific allocator methods.
- `Format<Crc = crc::NoCrc, MaxData = 252>` defines useful function-data capacity.
  MBAP, function and CRC are added automatically; smaller data limits restrict
  local capacity. Frames over 260 bytes are explicitly private
  exchanges, not standard Modbus. The encoding ceiling is 65541 bytes:
  six bytes before the 16-bit Length field's counted region, plus 65535.
- The default is standard Modbus TCP, with no trailer. An explicit CRC policy
  adds a PRIVATE extension, agreed at both ends, with no autodetection. The
  same `crc::Policy` and stateful calculator constructors as RTU are accepted;
  the library does not validate the algorithm. Unused tables emit no storage.
- No function-length framer, clock, HAL include,
  socket descriptor, connection manager or transaction scheduler in the core.
- `make_message(transaction_id, unit_id, function[, capacity_hint])` owns
  all MBAP fields. The hint and `size/capacity` count function-data bytes only.
- All existing `append_native/be/le/bytes` scalar/span forms are preserved.
  `read_native/be/le/bytes` are exact exports of `wire` readers.
- `Packet` is pointer-sized, copyable and immutable. `Message` is move-only.
  Both expose transaction/unit/function metadata; Packet also exposes
  `data/size/pdu/adu`. Storage and packet/message types are independent of RTU.
- `bind/unbind`, Sender/BusyQuery, `send`, `tx_active`, `poll(now_ms)`,
  `has_packet/pop_packet`, `stats/storage` keep their existing semantics.
  `Sent` transfers ownership, not proof of delivery. `Failed` preserves a
  byte-identical finalized message for retry. Empty/foreign messages are Invalid.
- `consume(span)` accepts every split/coalescing of ADUs, including empty
  chunks. All function and unit byte values are transported without function
  semantics. A client/server above this layer validates its application PDU.

## Wire and internal ownership

ADU: `[transaction:BE16][protocol:BE16=0][length:BE16][unit:u8][function:u8][data][optional CRC]`.
Length counts unit + function + data + trailer, not its own field or the full
ADU. With an explicit CRC extension it is filled BEFORE calculation, which
covers the complete MBAP + PDU, excluding only the trailer. Its width/order
come from the policy. Packet data/PDU exclude the trailer; ADU includes it.
The standard default permits 252 function-data bytes; its minimum ADU is 8.
In general the minimum is `8 + Crc::wire_size`, maximum data is `MaxData`,
and the derived maximum ADU is `MaxData + 8 + Crc::wire_size`. Layout is keyed only on trailer width and limit,
so Bitwise/Table of equal width share Geometry/Storage/Message/Packet types.
The six-byte fixed prefix is gathered before allocation. A validated Length
sizes the exact final allocation; unit/function arrive in the counted tail.
No unbounded staging buffer and no vector are owned by the parser.

The RX allocation contains a trivially destructible private header (refs,
32-bit ADU size, ready-queue link, exact storage owner), then the complete ADU.
Metadata access reads that immutable ADU rather than duplicating its fields.
Geometry publishes rounded RX bytes, maximum TX bytes and RX alignment.

TX keeps the original `TxBlock` unchanged. Overgrants are clamped only for
usable capacity; short grants are released and rejected. Growth allocates
first, copies header + existing data, then releases the original block.
Failure leaves the original ownership, data, size and capacity intact.

## Stream state and recovery

`Header -> Body -> Header`; allocation failure uses `Header -> Skip -> Header`.
An invalid protocol ID, Length below `2 + crc_size`, or ADU above the configured capacity
sets a fail-closed RX state before allocation. Subsequent input is ignored.
The endpoint reports `rx_failed()`; it never guesses a new MBAP boundary.
A CRC mismatch also fails closed: a corrupted but plausible Length could have
caused the apparent boundary itself to be wrong. MBAP determines lengths; it
is not a self-synchronizing delimiter and cannot repair dropped/inserted bytes.

`notify_gap()` releases only an incomplete RX block and poisons RX because
the next byte's alignment is unknown. `reset_rx()` starts a caller-guaranteed
new stream and clears the incomplete/failed parser state. It does not clear
ready packets, statistics or an outstanding TX borrow. Previously popped
Packets remain valid. A transport adapter must stop/close a failed stream
and may reset only at a known new stream boundary.

Valid headers rejected for OOM retain a known frame size: consume exactly the
remaining bytes without allocating, then parse the next ADU. Do not mistake
payload bytes containing a plausible MBAP header for another frame.

Endpoint/storage outlive all Packet/Message owners and every transport borrow.
All mutation and non-atomic packet references use one serialized execution
domain. The transport delegates do not throw or re-enter the endpoint.
Partial socket writes belong to a future adapter: Sender accepts the entire
borrow or none, never a prefix while returning false. This slice adds no socket
adapter and therefore makes no partial-write/reconnect implementation claim.

## Implementation phases and acceptance

1. Format, private RX header, Packet, Message and stream Receiver; Endpoint
   composes them without any transport headers. Default NoCrc emits no CRC work.
2. Independent wire vectors, capacity/overflow/ownership/API tests, arbitrary
   chunking, coalesced frames, malformed headers, poison/reset, allocation
   failure/skip, same-message growth retry and retained Packet tests. Include
   default, small and maximum private geometries; host sanitizers and O3/LTO.
3. Independent header and negative-compile guards, native Windows consumers,
   Cortex-M layout/codegen and unchanged COBS/RTU/shared regression suites.
4. Real H7S execution over UART: exact MBAP bytes, split/coalesced traffic,
   Pool/Heap, MCU-local negative ownership/framing/OOM tests, independent host
   byte oracle. Back up, restore and read back the original boot flash; retain
   source/image hashes and raw observations. No Ethernet/TCP/IP stack is started.
5. Usage/architecture/API-parity/build documentation names actual coverage,
   limits and future socket work. Do not relabel UART evidence as TCP sockets.

Source: [Modbus Messaging on TCP/IP Implementation Guide V1.0b, section 3.1](https://www.modbus.org/file/secure/messagingimplementationguide.pdf).

## Initial completion record (2026-09-12, before the data-limit API update)

The original ADU-limit implementation completed phases 1..5 with the numbers
below. The subsequent [payload-limit update](PAYLOAD_LIMITS.md) changes TCP
and RTU size-parameter units and carries its own fresh verification receipt.
Do not attribute these original source hashes/counts to the updated tree.
[Usage](../src/modbus/tcp/README.md),
[live raw evidence and reproduction](../src/modbus/tcp/tests/hardware/h7s/README.md)
and [parity](API_PARITY.md) record the delivered boundary. No network adapter
was added. COBS/RTU/UART production code and Cube-generated sources are unchanged.

- Host: 74056 core + 18284 advanced checks, ASan/UBSan and O3/LTO; independent
  headers and eight compile-fail boundaries; MinGW, MSVC x64/x86 and qmake.
- ARM: 72 NoCrc objects, eight Cortex-M cores, three optimization levels and
  three endian/access modes; explicit Bitwise/Table emission controls.
- H7S: six images, 1974 byte-exact UART exchanges, 32 live rejection trials,
  shared core and real Heap/Pool OOM tests; original boot flash restored and
  the complete 64 KiB read-back matched the backup SHA-256.
- Existing shared/CRC/COBS/RTU/UART/adapter host regressions and executable
  integration examples passed. No new socket/network execution is claimed.
