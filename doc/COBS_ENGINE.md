# COBS Engine — implementation contract

This document refines `UART_COBS_ARCHITECTURE.md` into the contract the
implementation must satisfy. Where the two disagree, **this document wins**:
the architecture document is the original design sketch and still shows a
virtual `IByteTx` and a single large templated `Cobs`, both of which the
decisions below replace.

Nothing here is implementation. It exists so that the implementation can be
reviewed against a written contract instead of against somebody's memory of a
conversation, and so that the invariants that are cheap to state now stay
provable later.

The byte transport underneath is settled and frozen: see `uart/Uart.h`. The
only two things COBS may assume about it are `tx_busy()` and `send(span)`.

---

## 1. Decision table

| Question | Decision |
| --- | --- |
| Transport binding | compile-time, constrained by a `ByteTransport` concept |
| Decoder | standalone **non-template** class, no allocator, no transport |
| RX decode | incremental, directly into the packet — no encoded staging buffer |
| Transport gap | `DropUntilDelimiter`, absorbed entirely by COBS |
| Bare `0x00` | synchronization / no-op, never a packet |
| Empty packet | `01 00` |
| Delimiter inside a block | frame discarded, decoder is immediately synchronized |
| Oversize / allocation failure | `DropUntilDelimiter` |
| Ready queue | intrusive, threaded through the packets themselves |
| RX lifetime | intrusive refcount, `PacketRef`, payload immutable after publication |
| Refcount | plain (single execution domain); no atomic policy in v1 |
| Allocator | compile-time type; **must satisfy** the protocol limit, never define it |
| `MaxDecodedSize` | COBS/protocol configuration, includes any future integrity trailer |
| TX ownership | move-only `CobsMsg`, exclusive until the transport accepts it |
| Transport busy before encoding | message stays `Building` |
| `send()` failure after encoding | message stays `Encoded`, the same wire frame is retryable |
| TX queue | none |
| TX completion | `proceed()` polls `tx_busy()` |
| CRC | not in v1, and free to add later **because** of the `MaxDecodedSize` definition |
| Observability | counters only; no hot-path instrumentation unless a probe is enabled |

---

## 2. Component split

```text
CobsDecoder            pure framing state machine
                       no allocator, no transport, no ownership
                       writes into a caller-supplied span

Cobs<Transport, Alloc> ownership, allocation, ready queue, TX state
                       thin glue over CobsDecoder
```

The split is not cosmetic. `Cobs` is templated on the transport and the
allocator, so an application with a `Uart<128, 8>` and a `Uart<256, 4>`
instantiates it twice. The decoder is where the subtle logic lives, and
duplicating it per instantiation would mean duplicating the code that most
needs to be reviewed and fuzzed exactly once. Keeping it non-template also
means it can be tested with no HAL, no pool and no transport at all — a plain
host binary that feeds it bytes.

### 2.1 The transport concept

```cpp
template<class T>
concept ByteTransport =
    requires(T& transport,
             const T& const_transport,
             std::span<const uint8_t> bytes)
{
    { const_transport.tx_busy() } noexcept -> std::same_as<bool>;
    { transport.send(bytes) }     noexcept -> std::same_as<bool>;
};
```

`tx_busy()` is required on a `const` reference deliberately: it is a query,
it is polled from `proceed()` on every loop iteration, and a transport that
needs to mutate itself to answer it is not the transport this design assumes.

Compile-time binding rather than a virtual interface is chosen for
`tx_busy()`, not for `send()`. On the measured H7S build `send()` costs 362
cycles, so a virtual call would disappear into the noise there. `tx_busy()`
is a single `volatile bool` read on the polling path; behind a vtable the
call would cost more than the operation it performs, and the compiler would
lose the ability to keep the idle path collapsed.

---

## 3. Wire format

A frame is a COBS-encoded byte sequence terminated by a single `0x00`
delimiter. No length prefix, no header, no leading delimiter is required
(though a leading delimiter is harmless — see below).

```text
frame ::= code_block+ 0x00

code_block ::= 0xFF <254 non-zero bytes>          no implicit zero
             | code <code-1 non-zero bytes>       implicit zero follows,
                                                  unless the delimiter is next
```

Three rules fix the semantics that the standard leaves to the implementation:

**A bare `0x00` is a no-op.** It does not produce a packet. This makes a run
of delimiters a harmless line flush:

```text
00 00 00 00      →  nothing delivered, decoder synchronized
```

It also makes a leading delimiter free, so an implementation that prefixes
frames for robustness interoperates with this one without special handling.

**An empty packet is `01 00`.** An application that deliberately sends a
zero-length packet has an unambiguous canonical representation, and it is
distinct from the no-op above.

**The encoder is canonical/minimal.** It never emits a redundant trailing
`01` block for payload lengths that are exact multiples of 254. Some COBS
implementations do; this one does not, which is what makes the size formula
in §4 a tight bound rather than a loose capacity estimate.

---

## 4. Size arithmetic

### 4.1 `MaxDecodedSize`

`MaxDecodedSize` is a **COBS/protocol configuration constant**. It is the
maximum size of a fully decoded frame — that is, everything that comes out of
the decoder, **including any future integrity trailer**, not just the payload
the application sees.

This wording is what makes deferring CRC free. When a CRC is added later, it
occupies decoded bytes:

```text
v1:      visible payload = MaxDecodedSize
later:   visible payload = MaxDecodedSize - CRC overhead
```

Block sizes, headroom and the overlap proof below are all expressed in
`MaxDecodedSize`, so adding an integrity trailer changes what the application
sees and nothing else. Had `MaxDecodedSize` been defined as "application
payload", every buffer in the system would have needed re-sizing the day a
CRC arrived.

The allocator does **not** define this limit. It must satisfy it:

```cpp
static_assert(Allocator::payload_capacity >= MaxDecodedSize);
```

The direction of that dependency is the point. If the limit came from the
allocator's block size, then swapping a fixed pool for a heap — or one pool
for another — would silently change which wire frames the protocol accepts.
The protocol states its requirement; the allocator either meets it or fails
to compile.

The same rule applies one level down, to how an allocator is configured. A
pool is parameterized by the **payload capacity it offers**, never by its raw
block size:

```cpp
template<std::size_t PayloadCapacity, std::size_t BlockCount>
class FixedPoolAllocator;

using RxPool = FixedPoolAllocator<MaxDecodedSize, 8>;
```

A block is a packet header followed by its payload, and the header's size is
an ABI property — 24 bytes on x86-64, 12 on Cortex-M. Parameterizing by block
size would make the same configuration accept different wire frames on
different platforms, which is the ABI deciding protocol semantics. Configured
this way, `payload_capacity` is exactly the number requested everywhere, and
the ABI only moves the RAM cost (`storage_size` = 1048 bytes per block on
x86-64 against 1036 on Cortex-M, for a 1024-byte capacity).

`storage_size` is the logical content of a block and is deliberately **not**
promised to equal the physical `sizeof(Block)`: alignment may add tail
padding.

The decoder itself knows neither constant. It is handed a
`std::span<uint8_t>` and the span's own extent is the limit it respects.

### 4.2 Encoded size

```cpp
constexpr std::size_t cobs_max_encoded_size(std::size_t n) noexcept
{
    return n == 0 ? 1u : n + (n + 253u) / 254u;   // n + ceil(n / 254)
}

constexpr std::size_t cobs_max_wire_size(std::size_t n) noexcept
{
    return cobs_max_encoded_size(n) + 1u;         // + delimiter
}
```

This is a **tight upper bound, not an exact length**. It is attained exactly
when the payload contains no zero bytes; payloads containing zeros encode
shorter, because a zero is consumed by the code byte that precedes it. Do not
use it to predict the length of a specific frame.

Worked values, all verified against the canonical encoder:

| payload | encoded | overhead | note |
| ---: | ---: | ---: | --- |
| 0 | 1 | — | the `01` of `01 00` |
| 3 (all zero) | 4 | 1 | trailing zero needs a following block |
| 254 | 255 | 1 | one `0xFF` block, no final code byte needed |
| 255 | 257 | 2 | `0xFF` block plus a two-byte block |
| 508 | 510 | 2 | two `0xFF` blocks |
| 509 | 512 | 3 | |

---

## 5. RX decoder

### 5.1 State

```text
Synced              between frames, waiting for a code byte
Decoding            inside a frame
DropUntilDelimiter  resynchronizing; every byte discarded until 0x00
```

```text
blockRemaining   bytes still expected in the current block
pendingZero      the current block ended with code != 0xFF, so an implicit
                 zero is owed — unless the next byte is the delimiter
```

### 5.2 Transitions

```text
                   +--------------------+
                   |       Synced       |
                   |  waiting for code  |
                   +---------+----------+
                        |         ^
              code != 0 |         | 0x00
                        v         |
                   +--------------------+
                   |      Decoding      |
                   +---------+----------+
                             |
        malformed / oversize / allocation failure / transport gap
                             v
                   +--------------------+
                   | DropUntilDelimiter |
                   +---------+----------+
                             |
                           0x00
                             v
                          Synced
```

In `Synced`, a `0x00` is consumed and ignored (§3).

### 5.3 The delimiter invariant

One variable separates a valid frame end from a corrupt stream, with no
heuristics:

```text
blockRemaining == 0  &&  byte == 0x00   →  valid frame delimiter, deliver
blockRemaining >  0  &&  byte == 0x00   →  malformed frame, discard
```

A data byte inside a COBS block is non-zero by construction, so a zero there
can only mean corruption or truncation. Both are handled identically.

### 5.4 Why a malformed frame does not enter `DropUntilDelimiter`

When the malformed condition is detected *because a delimiter arrived*, that
delimiter has already resynchronized the stream. Requiring another one would
discard the next perfectly good frame. So:

```text
oversize / allocation failure  (no delimiter seen yet)  →  DropUntilDelimiter
delimiter arrived too early    (delimiter consumed)     →  discard, Synced
```

### 5.5 The implicit zero

A block whose code is not `0xFF` owes a zero, but only if the frame
continues. The decoder therefore never writes it eagerly:

```text
block ends, code != 0xFF   →  pendingZero = true
next byte is a code byte   →  write 0x00, then process the code
next byte is the delimiter →  discard pendingZero, frame ends
code == 0xFF               →  pendingZero stays false
```

This is what makes the decoder indifferent to how the transport chops the
stream. The following must all produce the same packet:

```text
03 11 22 02 33 00        one span
03 11 | 22 02 | 33 00    three spans
0 3 | 1 1 | 2 2 | ...    one byte per span
```

Traced against the canonical encoding, this yields:

| wire | decoded |
| --- | --- |
| `01 00` | empty payload |
| `01 01 00` | a single `0x00` byte |
| `FF <254 bytes>` `00` | those 254 bytes, no trailing zero |

A materialized pending zero consumes output capacity like any other byte and
must be counted against the span extent before it is written.

---

## 6. RX allocation and delivery

### 6.1 Allocate on the first code byte

Nothing is allocated until a frame actually starts:

```text
Synced + code != 0x00
      → allocate
          success → Decoding
          failure → stats.allocation_failure++, stats.frames_lost++,
                    DropUntilDelimiter
```

`01 00` therefore allocates normally and delivers a zero-length packet. A dry
pool costs exactly one frame and no partial delivery: the decoder does not
try to salvage half a frame.

### 6.2 Intrusive ready queue

A completed packet is already an allocated object, so it is its own queue
node:

```text
RxPacket {
    refs
    size
    next_ready
    allocator ownership metadata
    payload
}

Cobs { readyHead, readyTail }
```

No second allocation, no fixed queue capacity to overflow, no dynamic queue
memory, O(1) push and pop. The queue length is bounded naturally by the
number of blocks the allocator owns — which is the property a separate
container would have destroyed, by making it possible for the queue to fill
while memory remained, or the reverse.

### 6.3 Ownership transfer on pop

The ready queue holds one reference. `pop_packet()` hands that reference to
the caller's `PacketRef`:

```text
queue owns refs = 1   →   pop   →   PacketRef adopts refs = 1
```

No increment and no decrement occurs during the pop; the reference moves.

### 6.4 `PacketRef` is allocator-typed

```cpp
template<class Allocator> class PacketRef;
```

and the packet knows its owner:

```cpp
if (--packet->refs == 0) {
    packet->owner->deallocate(packet);
}
```

No `void*` owner, no deleter function pointer stored per packet. Type-erasing
the deallocation would amount to re-implementing `shared_ptr` inside the
packet, which is what this design exists to avoid.

The refcount is a plain integer in v1. A single execution domain needs
nothing more, and an atomic refcount policy can be introduced when
cross-task sharing actually appears.

### 6.5 Immutability, stated precisely

> **Decoded payload bytes are immutable after publication; ownership and
> queue metadata (`refs`, `next_ready`, allocator bookkeeping) remain
> mutable internally.**

"The packet is immutable" is too strong and would eventually lead someone to
`const`-qualify a field the ready queue must write.

---

## 7. Transport gaps

The byte transport reports a discontinuity — bytes physically lost — in
order, at the point in the stream where it happened. COBS **consumes that
notification entirely**:

```text
transport gap
      → discard the frame being built (if any)
      → DropUntilDelimiter
      → the first 0x00 restores framing
      → stats.frames_lost++
```

A gap moves the decoder to `DropUntilDelimiter` **even when it appears to be
between frames**. After a physical loss there is no evidence that the next
byte is the start of a frame rather than the middle of one, and treating it
as a code byte is how a corrupted tail gets mistaken for a new packet.

**No gap callback is exposed above COBS.** Packets already in the ready queue
structurally predate the loss, and the only frame affected is the one being
built, which is dropped. The application simply does not receive that packet;
a counter records it. Propagating the gap upward would hand the application
an event it cannot correctly attach to any position in the packet stream, and
would re-open the ordering problem that the transport layer already solved.

---

## 8. TX

### 8.1 `CobsMsg` state machine

`CobsMsg` is move-only.

```text
Empty  --get_msg()-->  Building  --push()-->  Encoded  --send() ok-->  Transferred
                          ^                      |
                          |                      | send() failed
                          +--- transport busy ---+  (stays Encoded, retryable)
```

The `Encoded` state is load-bearing rather than decorative. `push()` first
checks `tx_busy()`, and a busy transport leaves the message `Building`,
untouched and still writable. But once encoding has run — in place, over the
caller's own bytes — the raw payload no longer exists. If `send()` then fails
to start the hardware, there is nothing to undo and nothing to re-encode:
the message stays `Encoded` and `push()` may retry the *same* wire frame.

`write()` and `reserve()` are rejected once the message is `Encoded`.

### 8.2 One active transfer

When `send()` succeeds, ownership moves:

```text
CobsMsg  →  Cobs::activeTx        (CobsMsg becomes empty)
```

and `proceed()` does only this:

```cpp
if (activeTx && !transport.tx_busy()) {
    release(activeTx);
    activeTx = nullptr;
}
```

The contract inherited from the transport, and it must be stated in the COBS
documentation too:

> `tx_busy() == false` means only that the transport has stopped borrowing
> the buffer. It is **not** a delivery acknowledgement.

A frame may have ended through the transport's error path and still release
its memory. COBS manages lifetime; delivery outcome is the transport's
business, reported through its own counters and handler.

Release is polled rather than driven from the transport's completion
callback. The callback runs in interrupt context, and deallocating there
would require an interrupt-safe allocator for no benefit beyond one loop
iteration of latency.

### 8.3 Zero-copy TX: one block, encoded in place

A TX block carries the raw payload at an offset, leaving headroom for the
COBS expansion in front of it:

```text
 block_start                     raw_start                        block_end
      |                              |                                 |
      +------------------------------+---------------------------------+
      |   raw_offset (headroom)      |   MaxDecodedSize                |
      +------------------------------+---------------------------------+
      ^                              ^
      encoder writes from here       application writes from here
```

Both quantities come from the size functions of §4.2 rather than from a magic
constant:

```text
wire_capacity = cobs_max_wire_size(MaxDecodedSize)
              = cobs_max_encoded_size(MaxDecodedSize) + 1

raw_offset    = wire_capacity - MaxDecodedSize
              = ceil(MaxDecodedSize / 254) + 1        (for MaxDecodedSize > 0)

block size    = wire_capacity
```

The block is exactly `wire_capacity` bytes: the worst-case encoded frame plus
its delimiter fills it precisely, ending where the raw region ended.

### 8.4 The overlap invariant, with its proof

The encoder reads raw bytes forward from `raw_start` while writing encoded
bytes forward from `block_start`. The invariant, in terms of bytes written
and raw bytes consumed:

```text
written <= consumed + raw_offset - 1     while unread raw data remains
```

Equivalently: the next byte written always lands strictly before the next
byte to be read. Once the input is fully consumed, the writer may use the
whole remaining region, including the delimiter position.

**Proof.** Let `d = written − consumed` after any prefix of the encoding. A
code byte adds one to `written` without consuming input; a data byte adds one
to each; a zero byte in the payload is consumed by the code byte of its block
and so adds one to `consumed` without adding to `written`. Therefore `d`
equals the number of code bytes written minus the number of zeros consumed,
and it is maximized by a payload containing no zeros at all.

For an all-non-zero payload of `N` bytes, code bytes are written at
`consumed = 0` and after each complete run of 254 bytes, so the maximum of
`d` over the whole encoding is

```text
max(d) = 1 + floor((N - 1) / 254)
```

Writing `N = 254q + r` with `0 <= r < 254`: for `r = 0` this is
`1 + (q − 1) = q = ceil(N/254)`; for `r >= 1` it is `1 + q = ceil(N/254)`.
So for every `N >= 1`

```text
max(written - consumed) = ceil(N / 254)  =  raw_offset - 1
```

exactly — the bound is attained, not merely respected. Substituting gives
`written <= consumed + raw_offset − 1`, which is the invariant. ∎

Two consequences worth keeping in the document, because both are easy to
"optimize" away later by someone who sees only the arithmetic:

- **The `+1` in `raw_offset` is load-bearing.** Without it the bound becomes
  `written <= consumed + raw_offset`, i.e. the writer lands exactly on the
  next unread byte. That is safe only if every byte is read before it is
  written, on every path, forever. With the `+1` there is one byte of
  separation, so an off-by-one in the encoder corrupts nothing silently — it
  fails a test instead.
- The bound is tight at several points, so the margin really is one byte and
  not an accident of rounding. Any future change to the encoder that emits an
  extra byte anywhere must revisit this proof.

---

## 9. Counters

Counters only — no hot-path instrumentation unless a probe is compiled in,
following the same rule as the transport layer.

```text
rx: frames_delivered, frames_lost, allocation_failure,
    malformed, oversize, resyncs
tx: frames_sent, send_refused_busy, send_failed
```

`frames_lost` is the single place a transport gap becomes visible to the
application, and it is deliberately a number rather than an event (§7).

---

## 10. Test plan

The decoder is a non-template class over a plain span, so all of this runs as
a host binary with no HAL, no pool and no transport.

### 10.1 Lengths

```text
0, 1, 253, 254, 255, 508, 509, MaxDecodedSize
```

### 10.2 Patterns, for every length above

```text
all non-zero
all zero
zero as the first byte
zero as the last byte
a zero every 254 bytes
alternating zero / non-zero
deterministic pseudo-random
```

### 10.3 Streaming boundaries

More important than a hundred whole-frame vectors: the same encoded input
must decode identically however it is split.

```text
whole frame in one span
one byte per span
split immediately before a code byte
split immediately after a code byte
split before the delimiter
split between the last data byte and the delimiter
```

### 10.4 Round trip

`decode(encode(x)) == x` for every length × pattern × boundary combination,
and `encode` output length `<= cobs_max_encoded_size(len)` with equality
exactly on the all-non-zero payloads.

### 10.5 In-place TX encoding

The overlapping encoder gets its own property test with sentinel bytes
surrounding the block, checked for every length in §10.1 and the lengths
adjacent to each of them. An overlapping encoder that is only tested on round
numbers has a remarkable talent for working until the first 254-byte customer
packet.

### 10.6 State machine

```text
gap between frames        → next frame after the delimiter is decoded
gap mid-frame             → partial frame dropped, next frame decoded
delimiter mid-block       → frame dropped, next frame decoded immediately
                            (no second delimiter required)
oversize                  → dropped, resync at the next delimiter
allocation failure        → dropped, counted, resync at the next delimiter
run of bare delimiters    → no packets delivered
leading delimiter         → harmless
```

---

## 11. Explicitly not in v1

- CRC / integrity trailer. Reserved by the `MaxDecodedSize` definition (§4.1);
  the decoder and the block layout do not change when it is added.
- Atomic refcount policy for cross-task packet sharing (§6.4).
- Any TX queue. Busy policy belongs to the layer above, exactly as it does
  for the byte transport.
