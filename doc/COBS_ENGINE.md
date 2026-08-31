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
| Transport binding | delegates (`set_sender` / `set_tx_busy`), so `Cobs` is templated on the allocator alone |
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
| Allocator | a compile-time **policy** (§9): one type, one template parameter, any backing memory. **Must satisfy** the protocol limit, never define it. Defaults to `CobsHeapAllocator`; embedded targets opt into a fixed pool |
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

Cobs<Allocator>        ownership, allocation, ready queue, TX state
                       thin glue over CobsDecoder; transport by delegate
```

The split is not cosmetic. `Cobs` is templated on the allocator, so an
application using two different allocators instantiates it twice. The decoder
is where the subtle logic lives, and duplicating it per instantiation would
mean duplicating the code that most needs to be reviewed and fuzzed exactly
once. Keeping it non-template also
means it can be tested with no HAL, no pool and no transport at all — a plain
host binary that feeds it bytes.

### 2.1 Binding the transport

The transport is attached with delegates, not as a template parameter:

```cpp
cobs.set_sender(tiny::delegate<bool(std::span<const uint8_t>)>{...});   // send()
cobs.set_tx_busy(tiny::delegate<bool()>{...});                          // tx_busy()
```

so `Cobs` is templated on the allocator alone. One instantiation then works
above a UART, a TCP socket or a test double without being recompiled per
transport, and the binding matches the house style: `Uart` already delivers
everything through `tiny::delegate`.

An earlier draft of this document argued for a compile-time `ByteTransport`
concept on the grounds that `tx_busy()` is "polled every loop iteration", so
an indirect call would cost more than the single `volatile bool` read it
guards. **That estimate was overstated**, and the correction is worth
recording because it is the kind of argument that sounds convincing while
being wrong by an order of magnitude.

`tx_busy()` is not polled every iteration. It is polled only while a transfer
is in flight:

```cpp
if (m_activeTx != nullptr && !tx_busy()) { release(m_activeTx); }
```

An idle link short-circuits on the null pointer and never calls it at all. On
the measured H7S build a 256-byte frame at 10 Mbaud occupies the line for
about 256 µs, so a 1 kHz main loop asks the question a handful of times per
frame — against a delegate call of two or three instructions. The transport
being a template parameter would buy nothing measurable and cost an
instantiation of the whole COBS layer per transport type.

`send()` was never the issue: it costs a measured 362 cycles, in which an
indirect call disappears entirely.

Whatever supplies those delegates must still honour the transport contract:
`tx_busy()` is a query with no side effects, and `tx_busy() == false` means
only that the transport has stopped borrowing the buffer — never that a frame
was delivered (§8.2).

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

This is a **tight upper bound, not an exact length**. A zero-free payload
attains it for every `N`, which is what makes the bound tight — but it is not
the only payload that does, and a payload containing zeros is not necessarily
shorter. The exact length is

```text
encoded = N + (number of 0xFF blocks) + (1 if the last block is not an 0xFF block)
```

so for every `N <= 254` the encoded length is exactly `N + 1` whatever the
data, zeros included: an 0xFF block needs a run of 254 non-zero bytes, which
cannot occur. `00 00 00` encodes as `01 01 01 01` — four bytes, the bound.
Only above 254 does the placement of zeros start to matter, because only there
can long non-zero runs force extra 0xFF blocks.

Do not use the bound to predict the length of a specific frame. For the
overlap proof in §8.4 only tightness matters: a zero-free payload is a
worst-case witness, and nothing depends on it being the unique one.

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

### 4.3 Template parameter order

`Cobs<MaxDecodedSize, Allocator>`, in that order. It reads in protocol order
and matches the rest of the codebase (`Uart<ChunkSize, ChunkCount>`,
`FixedPoolAllocator<PayloadCapacity, BlockCount>` — the shape before the
mechanism), but the binding reason is that it is the only order that can carry
a default: a template parameter with a default may not precede one without,
and `MaxDecodedSize` has no sensible default while the allocator does.

The default is `CobsHeapAllocator` (§9.2), so the common spelling stays one
argument, `Cobs<1024>`, and the embedded fixed pool is a deliberate opt-in.

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

## 9. Allocator policy

Memory is a **policy**: one type, one template parameter, arbitrarily many
implementations. `Cobs` never learns whether the bytes come from a heap, a
static pool, external SDRAM, a TLSF arena or a debug allocator that poisons
freed blocks — it asks for RX memory, returns RX memory, asks for TX memory,
returns TX memory. No virtuals and no runtime dispatch: the compiler welds the
protocol to the memory at instantiation.

The point of a single policy is that a user supplying their own memory writes
**one** implementation, not a matched pair. `MyRxAllocator` plus `MyTxAllocator`
would be two nearly identical bodies of code and two chances to get the same
thing wrong.

### 9.1 The contract

```cpp
struct SomeCobsAllocator {
    // What this policy can hold. NOT what the protocol accepts — see 9.2.
    static constexpr std::size_t rx_capacity = ...;
    static constexpr std::size_t tx_capacity = ...;

    [[nodiscard]] RxAllocation allocate_rx() noexcept;
    void deallocate_rx(RxPacket<SomeCobsAllocator>* packet) noexcept;

    [[nodiscard]] TxAllocation allocate_tx() noexcept;
    void deallocate_tx(std::byte* memory) noexcept;
};

struct RxAllocation {
    RxPacket<...>*     packet  = nullptr; // constructed, refs == 1
    std::span<uint8_t> payload{};         // where the decoder may write
};

struct TxAllocation {
    std::byte*  memory   = nullptr;
    std::size_t capacity = 0;
};
```

Both allocations report their region rather than letting the caller compute
it, so a policy is free to place the payload somewhere other than immediately
after the header — a heap policy may allocate them separately, a pool policy
keeps them in one block. Exhaustion is a null `packet` / `memory`, never an
error code: `if (!allocation.packet)` is the check either way.

Three obligations that are easy to get wrong:

- **`deallocate_rx` runs the packet's destructor.** The policy owns that,
  because only the policy knows whether the pointer is valid at all. A
  validating pool must refuse a foreign or already-freed pointer **before**
  running any destructor on it — tearing down an object on memory that may
  belong to somebody else is worse than the leak a refusal costs.
- **RX and TX quotas are independent.** A policy may share one backing store,
  but RX exhaustion must never starve TX. A link that cannot transmit because
  the application is holding received packets is a deadlock, not back-pressure.
- **A rejected deallocation leaks one block; it must never corrupt the
  allocator.** Losing a block is recoverable and countable. A corrupted free
  list is neither.

### 9.2 The policy advertises capacity; it does not define the limit

This is the rule of §4.1 restated where it is most tempting to break. It would
be natural to let `rx_capacity` be the maximum frame the protocol accepts, and
drop `MaxDecodedSize` entirely. That must not happen: swapping a policy with
4096-byte blocks for one with 1024-byte blocks would then silently change
**which wire frames are legal**, making the memory backend a protocol
decision.

So the protocol limit stays a parameter of `Cobs`, and the policy is checked
against it:

```cpp
template<std::size_t MaxDecodedSize, class Allocator = CobsFixedAllocator<MaxDecodedSize>>
class Cobs final {
    static_assert(Allocator::rx_capacity >= MaxDecodedSize);
    static_assert(Allocator::tx_capacity >= cobs_max_wire_size(MaxDecodedSize));
};
```

A default that names the earlier parameter is legal, so the common spelling
stays one argument:

```cpp
Cobs<1024> cobs{allocator};                       // default policy
Cobs<1024, SdramCobsAllocator> cobs{allocator};   // somebody else's memory
```

The default policy is **`CobsHeapAllocator`**, as
`UART_COBS_ARCHITECTURE.md` §1 has said from the start. A pool default would
look more embedded-minded and be worse in practice: it would plant static RAM
in every translation unit that merely names `Cobs<N>`, and it would force the
library to invent a block count on the user's behalf. A target where `malloc`
is unwelcome is exactly a target whose author chooses the allocator
deliberately:

```cpp
using Allocator = CobsFixedAllocator</* RX */ 1024, 8, /* TX */ 1024, 2>;
Allocator allocator;
Cobs<1024, Allocator> cobs{allocator};
```

`Cobs` also clamps what it hands the decoder to `MaxDecodedSize` even when the
policy returns a larger payload span, so a generous allocator cannot widen the
protocol by accident.

### 9.3 What the policy must not contain

```text
Cobs<MaxDecodedSize, Allocator>
        |                    |
     protocol             memory
        |                    |
  decoder / encoder     allocate / deallocate
  RX and TX state       geometry and limits
  ready queue           anything: heap, pool, SDRAM, external region
  PacketRef / CobsMsg
```

A policy that starts to know about encoding, decoder state, the ready queue or
`PacketRef` behaviour has stopped being a memory policy. Keeping it dumb is
what makes "write your own allocator" a small job rather than a diploma in
metaprogramming.

### 9.4 Transport is not part of it

The transport is bound separately (§2.1) and never travels through the
allocator: memory and byte movement are unrelated concerns, and coupling them
would mean a new allocator for every transport.

---

## 10. Counters

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

## 11. Test plan

The decoder is a non-template class over a plain span, so all of this runs as
a host binary with no HAL, no pool and no transport.

### 11.1 Lengths

```text
0, 1, 253, 254, 255, 508, 509, MaxDecodedSize
```

### 11.2 Patterns, for every length above

```text
all non-zero
all zero
zero as the first byte
zero as the last byte
a zero every 254 bytes
alternating zero / non-zero
deterministic pseudo-random
```

### 11.3 Streaming boundaries

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

### 11.4 Round trip

`decode(encode(x)) == x` for every length × pattern × boundary combination,
and `encode` output length `<= cobs_max_encoded_size(len)`, with the
all-non-zero payloads attaining it for every length (other payloads may attain
it too — see §4.2).

### 11.5 In-place TX encoding

The overlapping encoder gets its own property test with sentinel bytes
surrounding the block, checked for every length in §11.1 and the lengths
adjacent to each of them. An overlapping encoder that is only tested on round
numbers has a remarkable talent for working until the first 254-byte customer
packet.

### 11.6 State machine

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

## 12. Explicitly not in v1

- CRC / integrity trailer. Reserved by the `MaxDecodedSize` definition (§4.1);
  the decoder and the block layout do not change when it is added.
- Atomic refcount policy for cross-task packet sharing (§6.4).
- Any TX queue. Busy policy belongs to the layer above, exactly as it does
  for the byte transport.
