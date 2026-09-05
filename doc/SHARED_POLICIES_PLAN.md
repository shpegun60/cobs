<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Shared storage and integrity policies

Status: completed and verified in the working tree, 2026-09-05. This is the canonical continuation of the completed
[2026-09-01 refactor](COBS_REFACTOR_PLAN.md), not a claim that its old wire
or ABI results already cover these changes.

Baseline: `01e074f702db54686a03e5b7af8f9177ac72e5e1`.
CRC hardening checkpoint: `a1ac09c98863678cc15060712d79d78638e6c5c0`.
The interrupted agent's uncommitted migration is preserved and continued.

## Locked boundaries

- C++20, owning delegates, typed ownership, intrusive shared immutable RX
  packets and move-only TX messages remain. No C API, virtual dispatch,
  type-erased packet owners, universal protocol engine, or UART redesign.
- The public configuration is `Protocol::Endpoint<Memory, Format>`.
- `wire::Heap`, `wire::Pool<Rx, Tx>` and a user-written Memory specification
  are usable unchanged in both protocols. A specification provides
  `template<class Geometry> class For`; each endpoint owns its own instance.
  Sharing an external arena is an explicit custom-memory decision.
- Storage accepts only physical byte counts. Its four operations are
  `acquire_rx(bytes)`, `release_rx(pointer)`, `acquire_tx(bytes)` and
  `release_tx(TxBlock)`, all noexcept. RX returns an aligned pointer or null;
  TX returns `{memory, granted}` with granted >= requested or an empty block.
  Empty release is harmless. Live allocations do not overlap and remain
  valid until returned to their originating instance. Release does not allocate.
- Geometry has exactly three compile-time constants: `rx_block_bytes`,
  `tx_block_bytes`, `alignment`. RX bytes are an alignment-rounded upper
  bound on the largest physical request, suitable for a slot stride; actual
  heap requests stay exact. There is no minimum-size field. Slot structs
  carry alignment when a custom storage chooses its own byte capacity.
- Only the protocol constructs RX metadata. Packet, Message, Receiver,
  framing, queues and statistics stay protocol-specific.
- `crc::Policy` remains structural: value type, constexpr wire size,
  noexcept calculation, codec and result comparison. No algorithm, width,
  polynomial or semantic whitelist is imposed on custom policies. A wrapping
  sum and a hardware handle remain legal. Codec's four integer types are a
  convenience boundary, not a restriction on custom Policy value types.
- Each endpoint owns one `[[no_unique_address]]` CRC object reused by RX/TX.
  NoCrc and unused table specializations must not emit lookup storage. The
  same exact Table specialization used by both links emits at most one table.
- Layout is keyed on structural values only, not Bitwise/Table. Geometry
  identity must preserve identical Storage, Packet and Message types when
  only the calculator changes.

## Wire formats and sizes

### COBS target

`Format<Crc = crc::Crc16Bitwise, Rx = 255 - Crc::wire_size, Tx = Rx>` for
policies whose trailer fits that default body. Oversized custom trailers
require an explicit larger payload limit, with checked arithmetic.

The frame is `COBS(length_le | payload | trailer) | 00`. Length excludes
itself and includes the trailer. CRC covers payload only. Width of length is
one or two bytes, selected from the larger RX/TX body limit. Both body limits
must fit 65535. Packet data/size and Message capacity count payload only.

Default useful capacity is 253, trailer is two bytes, body limit is 255.
Explicit `Format<crc::NoCrc, 255>` reproduces v1 bytes. Explicit payload
limits are never reduced by the trailer; the library computes storage around
them. Invalid length is rejected before allocation when known from the
header. A CRC failure after the delimiter drops that frame without discarding
the following frame. A valid empty payload still carries the selected CRC.

Peers must have coordinated length width, checksum semantics and trailer
codec. There is no on-wire version identifier, CRC autodetection or fallback.
In particular an old NoCrc receiver can deliver new CRC bytes as payload;
mixed versions are not guaranteed to reject each other.

### RTU target

`Format<Crc = crc::Crc16Bitwise, MaxAdu = 256>`. MaxAdu is physical ADU size,
not payload. Useful capacity is `MaxAdu - 2 - Crc::wire_size`; all subtraction
is guarded, including MaxAdu 0/1. Maximum ADU is 65535 because metadata uses
uint16_t. Standard reference constants are separate from instance limits.

Smaller capacities do not change the wire format. Actual ADUs above 256 or
different checksum semantics are private RTU-like exchanges. The type of a
custom hardware calculator cannot prove its semantic compatibility.

RX still takes one whole candidate. The existing UART adapter must deliver
that candidate whole; CRC is not a replacement for framing and NoCrc cannot
reject arbitrary fragments on integrity grounds. Transport size, DMA/cache
alignment and UART chunk limits remain integration concerns.

## TX grants and hot paths

Store and release the original descriptor unchanged. Derive useful capacity
separately as the largest fitting payload, clamped to the Format limit.
Reject and return an undersized nonempty grant without changing the old
message. Growth acquires first, copies payload between its old/new offsets,
then returns the old block. Failed acquisition leaves size, data and capacity
unchanged. COBS's inverse geometry is evaluated at allocation/growth, not on
each append. Exact boundary tests cover the COBS 254-byte expansion steps.

## Execution checkpoints

1. [x] Close CRC contract findings: reflected Initial docs/check vectors,
   nothrow construction and comparison including proxy conversion,
   truthful type-alias documentation and sanitizer reporting.
2. [x] Complete shared storage, migrate tests/consumers and parameterize RTU
   without changing either protocol's existing bytes. No forwarding headers
   or protocol-specific storage/CRC alias namespaces remain.
3. [x] Add COBS CRC policy and switch default to CRC16/253, keeping explicit
   NoCrc v1 regression vectors. Update host hardware-peer scripts together.
4. [x] Update current API/storage/protocol guides and examples; preserve
   historical measurements with their original revisions clearly labeled.
5. [x] Verify host checked/sanitized and optimized suites, compile-fail and
   standalone headers, qmake consumers, ARM layout/codegen and H7S hardware.

## Required evidence

- Every built-in checksum and NoCrc, custom sum and stateful calculator in
  both protocols; failed/empty/max/oversized frames; no-allocation RX rejection
  where the protocol can know the fault before acquiring memory.
- Shared Heap/Pool conformance with both real geometries, per-slot alignment,
  independent quotas, non-overlap, descriptor identity and release checks
  under NDEBUG. Negative concepts reject runtime sizes and unrounded RX.
- Overgrant beyond maximum, undergrant, growth failure, retained packets,
  retries, transport borrowing and eventual release.
- Bitwise/Table type-identity assertions and no unused lookup symbols;
  combined multi-translation-unit Table usage does not duplicate tables.
- RTU RxBlock stays 16 bytes on ARM32 and Packet stays one pointer. Any
  other layout change is measured and explained, not hidden by disabling tests.
- Fresh COBS default and explicit legacy NoCrc H7S results. Old hardware
  reports are comparison oracles, not automatic certification of a new tree.

No checkpoint is complete merely because production headers compile. Record
the exact checks performed and any unavailable board/toolchain separately.

Completed evidence, exact commands, ABI changes and bounded claims are in
[SHARED_POLICIES_VALIDATION.md](SHARED_POLICIES_VALIDATION.md): host sanitizer
and optimized suites, qmake, MSVC x64/x86, 6360 ARM CRC objects, shared-table
linking, 85 live COBS records and 127 live RTU records. The board's initial
internal-flash image was restored and verified. No commit or push is implied
by this working-tree checkpoint.
