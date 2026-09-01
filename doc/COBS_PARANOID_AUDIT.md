<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# COBS paranoid correctness and hot-path audit

Status: complete

Audit date: 2026-09-01

Audited baseline: `0263f522e764b801999fcda5d20140049f16b64d`

Published implementation checkpoint: `ff1d3f756b311bbaaff29e5e29606748fd8bd0cf`

Scope: every active production file under `cobs/`, its public contracts,
storage extension boundary, ownership fields, host/ARM layouts, test runners,
negative API tests, qmake packaging, and the byte-hot RX/TX paths. Historical
files under `doc/old/` were treated as history, not as active implementation.

This is a fresh audit of the completed architecture refactor, not an argument
for replacing it. The wire format, owning delegates, typed storage, intrusive
packet ownership, zero-copy receive and in-place transmit decisions remain
locked by `COBS_REFACTOR_PLAN.md`.

## 1. Outcome

No wire-format change, allocation-policy change, delegate replacement,
compatibility alias, C facade, virtual dispatch, lock, or hidden heap use was
introduced.

The material changes are:

1. `Decoder::consume()` keeps the decoding working set in registers and
   publishes it only at event/input boundaries. The old loop loaded and stored
   object fields and tested the top-level state for every decoded byte.
2. `Decoder::prepare_output()` explicitly prepares a known first segment while
   synchronized. `Receiver` uses it for its permanent length buffer, removing
   one event/call round trip from ordinary frames without changing the older
   `attach_output()` answer-to-`NeedOutput` contract.
3. Receiver state has no duplicate declared-length field or header-attached
   bool. `Stage::{NeedHeader, Header, Body}` makes attachment explicit, and the
   private building `RxBlock::size` is the single validated declaration until
   publication.
4. Exhaustive differential codec testing, maximum-format TX testing, an
   optimized `-O3/-DNDEBUG` oracle build, checked libstdc++ mode, non-recovering
   sanitizers, and reproducible hot-path benchmarks are permanent parts of the
   tree.
5. Previously implicit transport-callback, counter-wrap, refcount-capacity and
   checked-pool complexity contracts are now explicit.

## 2. Ownership and lifetime proof

| Owner | State physically owned | Release boundary | Audit result |
|---|---|---|---|
| `codec::Decoder` | framing state, borrowed output span, segment totals, current COBS block | clears borrowed span on complete, malformed, or discard | one state machine; no allocator or retained input |
| `detail::Receiver` | decoder, local length bytes, explicit RX stage, building pointer, intrusive ready queue, RX counters | building/queued references return through the same storage instance | no duplicate declared length or queue owner |
| `RxBlock<Storage>` | refcount, declared/published size, ready link, typed storage owner | last `Packet` reference calls `owner->release_rx()` | one allocation contains header and payload |
| `Packet<Storage>` | one typed `RxBlock*` | last intrusive reference | copy increments, move transfers, adopt remains coordinator-only |
| `Message<Storage>` | typed storage pointer, one `TxBlock`, logical size, encoded size, explicit state | destructor/growth/Endpoint hand-off | strong growth guarantee and exact capacity return retained |
| `Endpoint<Storage>` | storage by value, receiver, paired owning delegates, one active TX descriptor, TX counters | `poll()` after busy becomes false | one active transport borrow; no split delegate state |
| `Pool` / `BlockPool` | independent RX/TX free lists and accounting | validated release | deterministic capacity; checked release remains enabled under `NDEBUG` |

The endpoint must still outlive every packet/message it created and every
active transport borrow. That is a fundamental owner-address precondition, not
something an internal refcount can repair after endpoint destruction.

## 3. File-by-file invariant audit

### `Format.h`

- RX and TX body limits remain directional.
- The larger limit chooses one shared 1- or 2-byte little-endian length field.
- Both limits remain at most `UINT16_MAX`.
- Header-inclusive COBS size arithmetic is statically guarded.
- Zero-length payload geometry remains valid and allocates real framing space.

### `Codec.h`, `Decoder.cpp`, `Encoder.cpp`

- Decoder input is borrowed only for one call; no input pointer is retained.
- A code byte that starts a frame is consumed exactly once.
- A data byte or implicit zero that needs another segment is not consumed.
- A delimiter at a data position is malformed and also immediately restores
  synchronization.
- A delimiter at a code position completes the frame and cancels the pending
  implicit zero.
- Empty attached spans remain distinguishable from no attached span.
- Prepared output remains borrowed across bare delimiters and is cleared by
  complete/malformed/discard transitions.
- The encoder remains canonical, forward, in place, and overlap-safe at the
  exact minimum headroom.
- The encoder was not rewritten into a `memchr` plus copy scheme: that would
  scan ordinary non-zero data twice and had no target proof.

### `Storage.h`, `BlockPool.h`

- `TxBlock` keeps pointer and granted payload capacity together across every
  acquire/grow/send/release transition.
- Heap RX allocations are header plus exactly the requested payload.
- Pool RX and TX quotas remain independent.
- Pool blocks are widened/aligned privately when a free-list pointer requires
  more space than the client-visible block.
- Foreign, interior and double releases are rejected before cleanup runs.
- `BlockCount` is now statically limited to what `PoolStats::in_use` can
  represent, and requested alignment gets a direct power-of-two diagnostic.
- Checked release is honestly documented as `O(BlockCount)`, not `O(1)` and
  not implicitly compiled out by `NDEBUG`.

### `Packet.h`

- Normal receive performs no refcount increment: one reference moves from
  building state to queue to returned packet.
- Only packet copies change the count; moves do not.
- Copy assignment increments before release, preserving alias/self-assignment.
- The count is deliberately non-atomic and single-domain.
- The fixed-width count's simultaneous-handle capacity is explicit; exceeding
  `UINT32_MAX` handles is outside the contract rather than silently accepted.

### `Receiver.h`

- The fixed length buffer is prepared while synchronized.
- After a gap/discard, preparation is deferred until the decoder reports the
  first new frame, because the next byte is not yet known to be a code byte.
- Non-empty allocation occurs only after a complete, in-range declaration.
- The decoder receives exactly the declared body span, never `rx_max_size`.
- The building block stores that validated declaration in its private `size`;
  publication still happens only after the delimiter and equality check.
- Failures discovered before the delimiter discard to the next delimiter;
  failures discovered by the delimiter do not throw away another frame.
- Ready queue head/tail updates remain centralized and O(1).
- Teardown releases both the building block and every queued reference.

### `Message.h`

- Empty, Building and Encoded remain explicit states.
- A failed append/growth preserves the old block, bytes, size and capacity.
- Growth acquires before copying/releasing and returns the old block with its
  own reported capacity.
- Scalar serialization rejects structs, bool, pointers, volatile values and
  bool-backed enums.
- Encoding writes `[length][payload]` in place, caches the exact wire span for
  retry, and never rewrites an already encoded frame.
- Narrowing the size fields was measured and rejected; smaller layout did not
  justify the repeatable TX cost. The original native-width fields remain.

### `Cobs.h`

- Storage remains the endpoint's sole template parameter.
- Sender and busy query remain paired owning `tiny::delegate` values.
- Bind/unbind remain transactional and are refused during active TX.
- Ownership moves from message only after sender acceptance.
- Idle `poll()` still short-circuits without invoking the busy delegate.
- Delegate targets must not throw or re-enter TX-control methods; sender return
  and busy-query values now have an explicit buffer-borrow meaning.

## 4. Performance evidence

### 4.1 Host benchmark protocol

Recorded environment:

- WSL2 Linux 6.6.87.2, x86-64;
- Intel Core i9-14900HX;
- GCC 13.3;
- `-O3 -DNDEBUG -march=native`;
- best of five, same machine/compiler/flags;
- approximately 16 MiB per sample for the four-pattern comparison.

Representative decoder latency (ns/frame):

| Payload / pattern | Baseline | Audited tree | Change |
|---|---:|---:|---:|
| 32, non-zero | 37.4 | 26.2 | -29.9% |
| 32, sparse zero | 44.0 | 30.9 | -29.8% |
| 256, non-zero | 248.3 | 139.6 | -43.8% |
| 256, sparse zero | 258.3 | 207.6 | -19.6% |
| 1024, non-zero | 870.3 | 531.1 | -39.0% |
| 1024, sparse zero | 930.2 | 816.3 | -12.2% |
| 1024, alternating zero | 914.0 | 601.4 | -34.2% |

Across all twelve measured size/pattern combinations, decoded-frame latency
fell by approximately 12-44%. Encoder production code is unchanged; encoder
timing movement between separately linked binaries is treated as layout/noise,
not as a claimed win.

The durable runner is `cobs/tests/bench/run.sh`. It also reports full fixed-pool
RX and TX paths. Those paths showed no material TX regression; end-to-end RX
gains depend strongly on COBS block distribution because allocation, queue and
packet-release work is unchanged.

### 4.2 Cortex-M code generation cost

Compiler: GNU Arm Embedded 14.3, Cortex-M4 soft-float, no exceptions/RTTI.

| `Decoder` build | Baseline consume | Audited consume | Delta | Stack baseline/current |
|---|---:|---:|---:|---:|
| `-O3` | 256 B | 316 B | +60 B | 32 / 40 B |
| `-Os` | 212 B | 270 B | +58 B | 32 / 32 B |

The flash/`-O3` stack increase is accepted because the generated hot loop keeps
output pointer/capacity, written count, block remainder and pending-zero state
in registers instead of touching decoder fields per byte. Object layout of
`codec::Decoder` itself remains unchanged.

### 4.3 RAM layout

| Type | x86-64 before/current | Cortex-M before/current |
|---|---:|---:|
| `Message<Heap>` | 48 / 48 B | 24 / 24 B |
| `Receiver<Heap>` | 136 / 120 B | 80 / 72 B |
| `Endpoint<Heap>` | 304 / 288 B | 168 / 160 B |
| `Endpoint<Pool<8,2,Format<1024>>>` | 10800 / 10784 B | 10592 / 10584 B |

The receiver reduction comes from removing duplicate state, not from packing
or inferring the state machine.

### 4.4 Real-silicon COBS + UART integration addendum

On 2026-09-01 the production COBS checkpoint above was integrated with the
audited UART tree at `ef0a78cd0ebdf0d9b4fbd99fe0e8c1ef81c9b628` and rerun on
a NUCLEO-H7S3L8 rev Y. The board exercised
`Uart<128,8>` and
`Endpoint<Pool<8,2,Format<1024>>>` through USART3/GPDMA and the ST-Link
VCP. The PC side used an independent COBS/length implementation rather than
linking the library under test.

The complete functional suite passed at 115200, 1M, 3M, 6M and 10 Mbaud:
81 boundary/pattern vectors at each rate, exact malformed/length/oversize
classification, TX Busy ownership retention, deterministic RX/TX pool
exhaustion, FIFO recovery, and pipelined full-duplex echo. Separate 115200 and
10M tests forced a physical UART overrun, observed one ordered COBS gap/resync,
supplied the required post-gap delimiter, and delivered the following packet
without resetting the MCU.

The final 30-second 10M/window-7 run echoed 61,235 frames and 19,015,336
payload bytes with zero unexpected UART, COBS, pool, ownership, or accounting
failure. Its non-overlapping data-driven regions consumed 5.929% of one
600 MHz M7 core. This includes full IRQs, UART slow service/COBS consume,
complete packet processing through checked RX release, and checked TX owner
release; nested sub-counters are not double-counted.

The exact harness contract, commands, acceptance rules, accounting formula,
reproduction command, result tables, and the 29-record raw JSONL are under
`cobs/tests/hardware/h7s/`. The matrix finished by flashing and smoke-checking
the default 115200-baud image. No production COBS or UART behavior was changed
to obtain this result.

## 5. Measured changes deliberately rejected

### Allocation bitmap

An O(1) live-block bitmap was prototyped for checked pool release. On the host,
acquire+release changed from about 0.65 to 2.75 ns for one block and 1.89 to
2.76 ns for eight blocks; it became beneficial only around 32 blocks
(11.34 to 2.74 ns). Typical project pools are two to eight blocks, so the
bitmap, extra RAM and layout change were rejected. A future large-pool policy
can revisit the threshold with target measurements.

### Byte-representation free-list link

Replacing the placement-constructed free-list pointer with `memcpy` simplified
strict cast/lifetime diagnostics but slowed the typical checked operation
(about 0.67 to 0.83 ns for one block and 1.90 to 2.20 ns for eight). The
existing aligned lifetime-starting implementation remains.

### Narrow message counters

Using 16/32-bit logical and encoded sizes reduced `Message` layout, but the
end-to-end TX benchmark repeatedly regressed, especially on larger frames.
Native `size_t` fields remain. Layout is not optimized at the expense of the
path the object exists to serve.

## 6. Verification added by this audit

- Exact differential transcript comparison for every 0-7 byte stream over
  `{00,01,02,03,55,FE,FF}`: 960,800 streams under ten input/output/prearm plans.
- Independent canonical encoder comparison: 177,146 payload/headroom cases.
- Pure codec coverage around decoded size 65,537.
- Full `Format<65535>` message build and canonical in-place send.
- Prepared-output semantics across leading delimiters and exhaustive malformed
  transitions.
- ASan+UBSan configured to stop at the first error, plus
  `_GLIBCXX_ASSERTIONS`.
- A separate optimized `-O3/-DNDEBUG` exhaustive codec executable.
- Strict GCC `-fanalyzer -Werror` and aggressive-warning compile of both
  non-template codec sources and a real public endpoint consumer.
- Repeatable direct-codec and complete-endpoint microbenchmarks.

## 7. Explicit residual contracts

These are not unresolved bugs; they are the boundaries required to keep the
implementation deterministic and fast:

1. Endpoint, decoder, storage and packet copies are single-execution-domain;
   there are no locks or atomic refcounts.
2. Endpoint outlives all packets/messages and every active transport borrow.
3. Delegate targets do not throw and do not re-enter endpoint TX-control
   methods.
4. A custom `Storage` implementation is trusted to satisfy its documented
   runtime capacity, non-overlap, lifetime and exact-release obligations; the
   C++ concept checks syntax and `noexcept`, not allocator truthfulness.
5. Protocol and pool event counters wrap modulo 2^32; monitoring extends
   periodic modular deltas if wider lifetime totals are needed.
6. Checked pool release is O(BlockCount). `COBS_POOL_CHECKS=0` is an explicit
   performance/safety trade, never an accidental consequence of `NDEBUG`.
7. A standalone decoder owner must impose its own decoded-size limit and stop
   supplying segments when that limit is reached. `Receiver` does this from
   `Format`; the generic codec intentionally has no protocol maximum.

## 8. Closure criteria

All verification below was repeated on the final implementation tree, and the
implementation checkpoint is present on the existing remote branch:

- [x] MinGW host suite, compile-fail contracts, headers, `NDEBUG`, optimized oracle;
- [x] WSL ASan/UBSan suite with non-recovering sanitizer mode;
- [x] exact x86-64 and Cortex-M layout assertions;
- [x] Cortex-M `-O3`/`-Os` codec warning and stack-usage builds;
- [x] qmake public consumer and root project clean builds;
- [x] NUCLEO-H7S3L8 end-to-end COBS/UART matrix through 10 Mbaud, including
      exact fault/pool/backpressure tests, physical gap recovery and 30-second
      maximum-rate stress;
- [x] `git diff --check` and a clean tracked build boundary;
- [x] scoped commit and push on the existing branch.
