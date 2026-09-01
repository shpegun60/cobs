# COBS Engine — implementation contract

> Refactor tracking: `COBS_REFACTOR_PLAN.md` records the locked decisions,
> target architecture, migration phases, and verification gates. This file
> remains the current behavioural contract until that migration is complete.

This document refines `UART_COBS_ARCHITECTURE.md` into the contract the
implementation must satisfy. Where the two disagree, **this document wins**:
the architecture document is the original design sketch and still shows a
virtual `IByteTx` and a single large templated `cobs::Endpoint`, both of which the
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
| Transport binding | delegates, bound as a PAIR by `bind`; `unbind` removes the pair explicitly, so `cobs::Endpoint` is templated on storage alone |
| Frame format | every engine frame is `COBS([length][body]) 00`; the length is fixed-width, little-endian, and counts only the body after it |
| Length width | 1 byte if `max(rx_max_size, tx_max_size) <= 255`, else 2. One width per engine, both directions; peers must agree |
| Decoder | standalone **non-template** class, no storage, no transport |
| Decoder output | SEGMENTED: `NeedOutput` may fire many times per frame, and `decoded_size` is the total across segments |
| RX decode | two-stage — header into a local 1–2 byte buffer, then the body straight into a packet allocated at exactly the declared length |
| RX acquisition size | `acquire_rx(declared_length)`; a 20-byte frame costs 20 payload bytes with `cobs::Heap` |
| Transport gap | `DropUntilDelimiter`, absorbed entirely by COBS |
| Bare `0x00` | synchronization / no-op, never a packet |
| Empty packet | a frame whose declared length is 0. `01 00` — a valid COBS frame with an empty decoded body — is an INVALID ENGINE FRAME, since it carries no length field: it is counted as `length_mismatch`, NOT as `malformed`, which stays reserved for structural COBS errors |
| Delimiter inside a block | frame discarded, decoder is immediately synchronized |
| Oversize / allocation failure | decided by `cobs::detail::Receiver` from the DECLARED length against `rx_max_size`, never by the decoder. `DropUntilDelimiter` only when the failure is found mid-frame; the two that are found AT the delimiter — an oversize header with no body, and a refused `acquire_rx(0)` — cost a frame and no resync (§6.1.2) |
| Ready queue | intrusive, threaded through the packets themselves |
| RX lifetime | intrusive refcount, `cobs::Packet`, payload immutable after publication |
| Refcount | plain (single execution domain); no atomic policy in v1 |
| Storage | a checked compile-time strategy (§9) and the single source of truth for memory strategy and quotas. It names one `Format`; `cobs::Endpoint<StorageT>` remains the whole engine signature. Defaults to `cobs::Heap<>`; embedded targets opt into `cobs::Pool` |
| Protocol limits | `StorageT::Format::max_receive_size` / `max_send_size`, both BODY limits — the decoded frame is `length_size` bytes longer. Republished by `cobs::Endpoint` under the same names |
| TX ownership | move-only `cobs::Message`, exclusive until the transport accepts it |
| Transport busy before encoding | message stays `Building` |
| `send()` failure after encoding | message stays `Encoded`, the same wire frame is retryable |
| TX queue | none |
| TX completion | `poll()` invokes the bound busy query while a transfer is active |
| CRC | not in v1, and free to add later **because** the declared length counts the whole body, trailer included |
| Observability | counters only; no hot-path instrumentation unless a probe is enabled |

---

## 2. Component split

```text
cobs::codec::Decoder   pure framing state machine
                       no storage, no transport, no ownership
                       writes into a caller-supplied span

cobs::Endpoint<StorageT>         ownership, acquisition, ready queue, TX state
                       thin glue over cobs::codec::Decoder; transport by delegate
```

The split is not cosmetic. `cobs::Endpoint` is templated on storage, so an application
using two different strategies instantiates it twice. The decoder
is where the subtle logic lives, and duplicating it per instantiation would
mean duplicating the code that most needs to be reviewed and fuzzed exactly
once. Keeping it non-template also means it can be tested with no HAL, no pool
and no transport at all — a plain host binary that feeds it bytes.

### 2.1 Binding the transport

The transport is attached with delegates, not as a template parameter — both
at once, never separately:

```cpp
cobs.bind(
    tiny::delegate<bool(std::span<const uint8_t>)>{...},   // send()
    tiny::delegate<bool()>{...});                          // tx_busy()
```

Two independent setters would make an inconsistent pair reachable while the
link is idle — a sender bound to one transport and a busy query still answering
for another. A `send()` would then start a transfer on one link while
`poll()` asked the other, which reports idle, and the block would be freed
while it is still being read. Taking both together makes that unrepresentable,
including the half-bound states: `bind()` refuses either empty delegate.
`unbind()` is the sole explicit removal operation. Binding, rebinding, and
unbinding are all refused while a transfer is in flight.

Because the transport is not a template parameter, `cobs::Endpoint` is templated on
storage alone, and one instantiation works above a UART, a TCP socket or a
test double without being recompiled per transport. The binding also matches
the house style: `Uart` already delivers everything through `tiny::delegate`.

An earlier draft of this document argued for a compile-time `ByteTransport`
concept on the grounds that `tx_busy()` is "polled every loop iteration", so
an indirect call would cost more than the single `volatile bool` read it
guards. **That estimate was overstated**, and the correction is worth
recording because it is the kind of argument that sounds convincing while
being wrong by an order of magnitude.

`tx_busy()` is not polled every iteration. It is polled only while a transfer
is in flight:

```cpp
if (m_activeTx.memory != nullptr && !tx_busy()) {
    m_storage.release_tx(m_activeTx);
}
```

An idle link short-circuits on the null pointer and never calls it at all. On
the measured H7S build a 256-byte frame at 10 Mbaud occupies the line for
about 256 µs, so a 1 kHz main loop — period 1 ms — usually sees the transfer
already finished: zero or one poll while it is in flight, plus the one that
observes completion. Two or three would need a loop running at roughly 10 kHz.
Against that, a delegate call of two or three instructions is not a cost worth
a template parameter, let alone an instantiation of the whole COBS layer per
transport type.

`send()` was never the issue: it costs a measured 362 cycles, in which an
indirect call disappears entirely.

Whatever supplies those delegates must still honour the transport contract:
`tx_busy()` is a query with no side effects, and `tx_busy() == false` means
only that the transport has stopped borrowing the buffer — never that a frame
was delivered (§8.2).

---

## 3. Wire format

An engine frame is a COBS-encoded `[length][body]` terminated by a single
`0x00` delimiter. No leading delimiter is required (though a leading delimiter
is harmless — see below).

```text
wire   ::=  COBS( length body ) 0x00
length ::=  1 or 2 bytes, little-endian, counting ONLY the body after it
body   ::=  the application payload (plus any future integrity trailer)
```

The length never counts itself. In v1 the body IS the application payload; a
future CRC would live inside the body and would therefore be included in the
declared length, which is the whole reason for defining it this way before the
trailer exists rather than after.

**The width is a wire-format property, not a memory one.** It is chosen from
the LARGER of the two directional limits, so one engine speaks one header
width in both directions:

```text
length_size = max(rx_max_size, tx_max_size) <= 255 ? 1 : 2
```

Choosing it per direction would let an engine with `rx_max_size = 1024` and
`tx_max_size = 64` expect two bytes and send one — a wire disagreement with
itself. It does not merge the limits: that engine still refuses to receive
above 1024 and to send above 64. A complementary pair agrees automatically:

```text
Peer A: RX 1024, TX 64    -> length_size 2
Peer B: RX 64,   TX 1024  -> length_size 2
```

Peers with different `length_size` are wire-incompatible even for a one-byte
frame. `cobs::Endpoint<A>::length_size` is constexpr so an integration build can
static_assert the format it expects.

Below the length field, the framing rules are unchanged:

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

**An empty packet declares a length of zero.** Its decoded content is the
length field and nothing else, so its wire form is the COBS encoding of one or
two zero bytes plus the delimiter. It remains distinct from the no-op above.

Note the consequence, which is a deliberate break with the pure-COBS layer
underneath: `01 00` — a structurally valid COBS frame with an EMPTY decoded
body — is no longer a valid engine frame, because it contains no length field.
`cobs::codec::Decoder` still accepts it as a zero-byte decoded frame; `cobs::detail::Receiver` counts it
as a length mismatch. The two layers are answering different questions, and
that is the point of keeping them apart.

**The encoder is canonical/minimal.** It never emits a redundant trailing
`01` block for payload lengths that are exact multiples of 254. Some COBS
implementations do; this one does not, which is what makes the size formula
in §4 a tight bound rather than a loose capacity estimate.

---

## 4. Size arithmetic

### 4.1 The two protocol limits

There are two, one per direction, and they are stated by `cobs::Format`. The
storage strategy names that type but does not duplicate its values (§9):

```text
max_receive_size    the largest BODY this instance will receive
max_send_size       the largest BODY the TX builder will carry
```

Both are **body sizes** — the decoded frame is `length_size` bytes longer, and
neither limit includes the header. They do include **any future integrity
trailer**, because the trailer lives inside the body (§3). That is what makes
deferring CRC free: when one is added it occupies body bytes at both ends,

```text
v1:      visible payload = rx_max_size          / tx_max_size
later:   visible payload = rx_max_size - CRC    / tx_max_size - CRC
```

and every buffer keeps its size.

**Neither limit is the size of a buffer any more.** Both directions size their
storage per frame, from the length prefix (§3):

```text
RX   the declared body length N, known from the header before the body
     arrives, so acquire_rx(N) is exact; rx_max_size is the CEILING above
     which a declared length is refused

TX   the capacity C granted for that allocation, C <= tx_max_size

     block size  = cobs::codec::max_wire_size(length_size + C)
     headroom    = cobs::codec::raw_offset(length_size + C)
```

Both formulas are header-inclusive because the header is part of what gets
COBS-encoded. `tx_max_size` is the ceiling the builder refuses to grow past,
not the size of anything.

Each limit is republished by `cobs::Endpoint` as `max_receive_size` and `max_send_size`.
Peers do NOT have to declare the same numbers — an asymmetric pair is the
normal case, and §3's example is exactly that. Three different conditions are
worth keeping apart, because only the first is about the wire:

```text
WIRE COMPATIBILITY      A.length_size == B.length_size
                        without this nothing decodes at all, in either
                        direction, not even a one-byte frame

PER-FRAME ACCEPTANCE    declared_length <= receiver.rx_max_size
                        decided per frame, from the header, by the receiver

FULL-RANGE COMPATIBILITY  A.tx_max_size <= B.rx_max_size
                          B.tx_max_size <= A.rx_max_size
                          only needed to guarantee that EVERY frame one side
                          is allowed to build is one the other will accept
```

The last is a design property, not a requirement. A peer with
`tx_max_size = 1024` talking to one with `rx_max_size = 64` is perfectly
functional as long as it keeps its frames under 64; the mismatch only means
the protocol has no static guarantee of that, and oversize frames will be
counted rather than delivered. The limits are each peer's own statement about
what it is willing to send and to accept, and may be larger than anything
actually used.

Neither republished name is `max_decoded_size`: that name was one header away
from the truth, on a layer where being one header out is the easiest mistake
there is.

```cpp
namespace cobs {

template<class StorageT = cobs::Heap<>>
class Endpoint final {
    using Format = typename StorageT::Format;
    static constexpr std::size_t max_receive_size = Format::max_receive_size;
    static constexpr std::size_t max_send_size    = Format::max_send_size;
};

} // namespace cobs
```

`Format` is now a first-class protocol type. Replacing heap storage with a pool
while keeping the same `Format` cannot change the header width or directional
limits. A storage implementation that names no `Format` does not satisfy the
checked `cobs::Storage` contract.

The rule that survives unchanged is the one a level down, about how a pool is
configured. A pool is parameterized by the **payload capacity it offers**,
never by its raw block size:

```cpp
namespace cobs {
template<class Format, std::size_t RxBlocks, std::size_t TxBlocks>
class Pool;
}
```

A block is a packet header followed by its payload, and the header's size is
an ABI property — 24 bytes on x86-64, 12 on Cortex-M. Parameterizing by block
size would make the same configuration accept different wire frames on
different platforms, which really would be the ABI deciding protocol
semantics — a machine property leaking into the protocol, as opposed to a
choice the author made on purpose.

Configured this way, both declared limits mean exactly what they say on every
ABI, and the ABI moves only the RAM cost. The two pools are sized differently
because they hold different things:

```text
RX block   sizeof(cobs::RxBlock<StorageT>)
             + rx_max_size                     1048 bytes on x86-64,
                                               1040 on Cortex-M, for 1024
TX block   cobs::codec::max_wire_size(length_size
             + tx_max_size)                    1032 bytes anywhere, since
                                               it contains no C++ object
```

Note that a TX block's size does NOT vary by ABI: it is bytes on a wire plus
headroom, with no header in it. Only RX carries an object whose size the ABI
decides.

### 4.2 Encoded size

```cpp
constexpr std::size_t cobs::codec::max_encoded_size(std::size_t n) noexcept
{
    return n == 0 ? 1u : n + (n + 253u) / 254u;   // n + ceil(n / 254)
}

constexpr std::size_t cobs::codec::max_wire_size(std::size_t n) noexcept
{
    return cobs::codec::max_encoded_size(n) + 1u;         // + delimiter
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

### 4.3 One template parameter

```cpp
namespace cobs {

template<class StorageT = cobs::Heap<>>
class Endpoint;

} // namespace cobs
```

That is the whole signature. The transport arrives by delegate (§2.1), and the
storage names its protocol `Format`, so there is no second engine parameter
that can disagree with it:

```cpp
cobs::Endpoint<> cobs;                        // cobs::Heap with the default Format
cobs::Endpoint<MyStorage> cobs;               // custom storage for its Format
```

Storage lives INSIDE the engine, by value (§9.4), so there is no separate
object to hand in. Storage needing runtime arguments is constructed in place:

```cpp
cobs::Endpoint<MyStorage> cobs{std::in_place, region_base, region_size};
```

Every component underneath refers to `StorageT::Format`; no layer reconstructs
wire geometry from storage constants.

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
        malformed / owner refuses another segment / transport gap
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

**Output is segmented.** `NeedOutput` is not a once-per-frame event: the
decoder raises it when a frame starts, and again whenever the attached segment
is full and another decoded byte is due. The owner answers with the next
segment or discards. `decoded_size` on `FrameComplete` is the total across all
of them.

```text
NeedOutput  -> attach segment A   (the length field)
A fills
NeedOutput  -> attach segment B   (the packet, sized from the declared length)
B fills or the delimiter arrives
FrameComplete(total)
```

Three rules make that safe:

- a `NeedOutput` raised for a full segment does **not** consume the encoded
  byte that still needs a home, so nothing is lost at a boundary — including
  an implicit zero owed across one;
- if the delimiter arrives exactly when the segment is full, the frame
  completes without asking for a segment nobody would use;
- `attach_output` is ignored while the current segment still has room, since
  replacing it would silently strand the bytes already written into it.

There is deliberately **no `Oversize` event**. "Too big" is not a fact this
class can know: it has no protocol limit, only a segment that happens to be
full, and a full segment is a request rather than a verdict. The layer that
knows `rx_max_size` and the declared length decides that — see §6.1.1. An owner
that will not grow expresses it by discarding, never by attaching an empty
segment, which would simply be asked again forever.

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
length disagreement found before the delimiter  →  DropUntilDelimiter
delimiter arrived too early (delimiter consumed) →  discard, Synced
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

### 6.1 Allocate once the length is known

Nothing is allocated when a frame starts. The first output segment is the
length field, which lives in one or two bytes inside `cobs::detail::Receiver` itself:

```text
Synced + code != 0x00
      → NeedOutput  → attach the local length buffer   (no allocation)
      length buffer fills, more decoded bytes due
      → parse the declared length N
          N > rx_max_size  → stats.oversize++,           DropUntilDelimiter
          N == 0           → stats.length_mismatch++,    DropUntilDelimiter
          acquire_rx(N)
              success      → attach exactly N bytes of the packet
              failure      → stats.allocation_failure++, DropUntilDelimiter
```

So a frame that lies about its size, or is larger than this instance accepts,
costs no allocation at all — it is refused from two bytes of evidence rather
than after filling a buffer.

`01 00`, whose decoded content is empty, therefore delivers nothing: it has no
length field, and is counted as a length mismatch (§3). A genuinely empty
application packet declares zero and is delivered as a zero-length packet.

A dry pool costs exactly one frame and no partial delivery: the decoder does
not try to salvage half a frame.

### 6.1.1 Exactly one copy-free path

The two-stage arrangement exists to keep RX zero-copy while making it
size-aware, which without a length prefix are mutually exclusive:

```text
frame starts
  → decode the header into 1-2 local bytes      the ONLY temporary storage
  → declared length N
  → acquire_rx(N)                              exact, once
  → decode the body straight into the packet    no staging buffer
  → delimiter
  → N == actual ? publish : free and drop
```

No payload byte is ever written twice, and nothing is copied after the
allocation.

### 6.1.2 Length validation, exhaustively

Every disagreement between a declared length and a frame, and what it costs:

```text
declared == actual                  publish

no header (decoded frame empty)     length_mismatch, frame lost
header truncated (1 of 2 bytes)     length_mismatch, frame lost
declared 0, body bytes follow       length_mismatch, resync
declared > rx_max_size, body began  oversize,        resync
declared > rx_max_size, no body     oversize,        frame lost
actual < declared                   length_mismatch, frame lost
actual > declared                   length_mismatch, resync
acquire_rx(N) fails, N > 0         allocation_failure, resync
acquire_rx(0) fails                allocation_failure, frame lost
```

Two of those pairs are the same verdict reached at different moments, and the
difference is only ever the resync. An oversize header is oversize whether or
not a body followed — the decision is made from the header both times, so a
frame declaring 65 against a limit of 64 lands on the same counter with one
body byte or with none. What changes is that a body means the rest of the
frame is still in the stream and must be skipped, while a delimiter arriving
first has already synchronized us. The same is true of a refused allocation:
for a non-empty body it happens mid-frame, while the empty packet is allocated
at FrameComplete, after the delimiter.

The resync column is not decoration. A failure the DELIMITER revealed — a
missing header, a short body — is already synchronized, so hunting for another
delimiter would throw away a good frame. A failure found BEFORE the delimiter
— an over-long body, an oversize declaration, a failed allocation — leaves the
rest of the frame in the stream, so it must be discarded to the next
delimiter and counted as a resync.

### 6.2 Intrusive ready queue

A completed packet is already an allocated object, so it is its own queue
node:

```text
cobs::RxBlock<StorageT> {
    refs
    size
    next_ready
    typed storage ownership metadata
    payload
}

cobs::Endpoint { readyHead, readyTail }
```

No second allocation, no fixed queue capacity to overflow, no dynamic queue
memory, O(1) push and pop. The queue length is bounded naturally by the
number of blocks storage owns — which is the property a separate
container would have destroyed, by making it possible for the queue to fill
while memory remained, or the reverse.

### 6.3 Ownership transfer on pop

The ready queue holds one reference. `pop_packet()` hands that reference to
the caller's `cobs::Packet`:

```text
queue owns refs = 1   →   pop   →   cobs::Packet adopts refs = 1
```

No increment and no decrement occurs during the pop; the reference moves.

### 6.4 `cobs::Packet` is storage-typed

```cpp
namespace cobs {
template<class StorageT> class Packet;
}
```

and the packet knows its owner:

```cpp
if (--block->refs == 0) {
    block->owner->release_rx(block);
}
```

No `void*` owner, no deleter function pointer stored per packet. Type-erasing
release would amount to re-implementing `shared_ptr` inside the
packet, which is what this design exists to avoid.

The refcount is a plain integer in v1. A single execution domain needs
nothing more, and an atomic refcount policy can be introduced when
cross-task sharing actually appears.

### 6.5 Immutability, stated precisely

> **Decoded payload bytes are immutable after publication; ownership and
> queue metadata (`refs`, `next_ready`, typed storage owner) remain
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

### 8.1 `cobs::Message` state machine

`cobs::Message` is move-only.

```text
Empty  --make_message()-->  Building
Building --append/reserve-----------------------------> Building
Building --send(): Busy or Unbound-------------------> Building
Building --send(): encode, sender refuses-----------> Encoded
Encoded  --send(): Busy, Unbound, or sender refuses--> Encoded
Building/Encoded --send(): sender accepts-----------> Empty
                                                      + Endpoint::activeTx owns block
```

The `Encoded` state is load-bearing rather than decorative. `send()` first
checks the busy query, and a busy transport leaves the message `Building`,
untouched and still appendable. But once encoding has run — in place, over the
caller's own bytes — the raw payload no longer exists. If the sender then fails
to start the hardware, there is nothing to undo and nothing to re-encode:
the message stays `Encoded` and `send()` may retry the *same* wire frame.

`Endpoint::send(Message&)` reports the ownership transition precisely:

| Result | Meaning | Message after return |
|---|---|---|
| `Sent` | the sender accepted the frame | empty; `Endpoint` owns the block |
| `Busy` | an active transfer exists or the busy query reports busy | unchanged in its current state |
| `Unbound` | no complete sender/busy-query pair is bound | unchanged |
| `Failed` | the sender refused after encoding | owns the same retryable `Encoded` frame |
| `Invalid` | empty message or a message from another endpoint | unchanged |

Every builder method — both `append_native()` overloads, `append_bytes()`, and
`reserve()` — returns `false` once the message is `Encoded`: the raw bytes they
would have appended to no longer exist.

### 8.2 One active transfer

When `send()` succeeds, ownership moves:

```text
cobs::Message  →  cobs::Endpoint::activeTx        (cobs::Message becomes empty)
```

and `poll()` does only this:

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
callback. The callback runs in interrupt context, and releasing there would
require interrupt-safe storage for no benefit beyond one loop
iteration of latency.

### 8.3 Zero-copy TX: one block, encoded in place

A TX block carries the raw payload at an offset, leaving headroom for the
COBS expansion in front of it:

```text
 block_start             frame_raw    raw_start                 block_end
      |                      |            |                         |
      +----------------------+------------+-------------------------+
      | cobs::codec::raw_offset(H+C) |     H      |  C, the granted capacity|
      +----------------------+------------+-------------------------+
      ^                      ^            ^
      encoder writes here    length header, written just before encoding
                                          the payload area starts here
```

`H` is `length_size` (§3). The header lives INSIDE the block, ahead of the
payload, and is invisible to `size()` and `capacity()` — those count
application bytes only. Every piece of geometry below is therefore in
`H + C`, not `C`:

```text
block size  = cobs::codec::max_wire_size(H + C)
frame_raw   = block + cobs::codec::raw_offset(H + C)      where the length goes
payload     = frame_raw + H                       what append operations fill
```

Both quantities come from the size functions of §4.2 rather than from a magic
constant, and both are **per message**: the block is sized for the capacity
this message was granted (§9.1.0), not for the largest Format allows.

`cobs::Message` is a CONTAINER, and carries three numbers. Confusing any two of them
is a bug:

```text
capacity()   payload bytes the current block permits; granted by storage and
             returned inside the same TxBlock descriptor
size()       payload bytes actually APPENDED; what coordinator encoding frames
m_wire       the encoded frame length, once coordinator encoding has run
```

`make_message(hint)` sets `size()` to zero and asks storage for `hint` bytes of
capacity. The hint is a hint: it spares growths, and nothing else. Bytes arrive
through `append_native(value)`, `append_native(span)`, and `append_bytes()`,
each of which grows the block when it has to.

```text
make_message()     cobs::Endpoint::default_capacity_hint — a practical reserve
make_message(0)    a zero initial capacity REQUEST; sent straight away it
                   sends the canonical empty frame, but it may still accept
                   appends and grow like any other message
make_message(N)    the caller knows a useful number
```

`default_capacity_hint` is **32**, and not zero, for a measured reason. A
message built field by field from a capacity of zero walks the whole ladder
below; with `cobs::Heap` that is 14 allocations and 453 requested bytes for a
100-byte payload. From 32 the same payload costs four allocations, and a
typical short frame costs one. It is free with `cobs::Pool`, which
reports `tx_max_size` whatever it was asked for. The value is clamped to
`tx_max_size`, so `make_message()` can never fail because of its own default.

An API that performs well only when the caller remembered to pass a hint is a
trap with good documentation.

### 8.3.1 Growth

Capacity grows geometrically, about 1.5x, computed by the CONTAINER — never by
storage (§9.1.0):

```text
delta  = max(1, capacity >> 1)
grown  = min(capacity + delta, tx_max_size)
target = min(max(required, grown), tx_max_size)

0 -> 1 -> 2 -> 3 -> 4 -> 6 -> 9 -> 13 -> 19 -> ...
```

`capacity >> 1` is written as a shift to make the 1.5x arithmetic obvious at a
glance, not to dodge a division: an optimizing compiler strength-reduces
`capacity / 2` to the very same `lsrs`, verified byte-for-byte on Cortex-M0 at
`-Os` and `-Oz`.

`max(1, ...)` is NOT load-bearing for progress, and an earlier revision of this
section wrongly said it was. `grow_target` is only reached when
`required > capacity`, and the final `max(required, ...)` therefore guarantees
a result above the current capacity whatever delta comes out — a delta of zero
would still advance. The clause stays because a geometric policy that stops
being geometric at capacity 1 is a special case waiting to surprise somebody.

A large jump is honoured in ONE allocation rather than by walking the sequence:
from a capacity of 64, a 500-byte append asks for 500, not 96 then 144 then
216. A request past `tx_max_size` is refused outright; nothing is clamped
silently.

The single-slab `cobs::Pool` reports `tx_max_size` from the first
acquisition, so with that strategy the growth path is reachable but never taken: a
message can be created with no hint at all and filled to the protocol limit
with one allocation, no reallocation and no copy.

**Reallocation is the one place a copy is allowed in the TX vertical**, and
only on an actual growth. Its order matters, because a failure must change
nothing:

```text
1. acquire the new block
2. if that fails -> return false; old block, size and contents all intact
3. copy size() payload bytes from old raw() to new raw()
4. release the old block WITH ITS OWN capacity
5. install the new pointer and the new capacity
```

The copy crosses two different offsets — `cobs::codec::raw_offset(old)` to
`cobs::codec::raw_offset(new)` — because the headroom a block needs depends on its
capacity. Headroom itself is never copied.

### 8.3.1.1 What the serializers accept

The scalar and span overloads of `append_native()` share ONE internal
constraint — `cobs::detail::NativeScalar` — so there is one rule to explain
rather than two nearly identical ones:

```text
append_native(value)  arithmetic (except bool), enumerations, std::byte
append_native(span)   a contiguous run of the same
append_bytes()        arbitrary bytes
```

Everything else is a compile error, deliberately:

- **Structs.** `struct { uint8_t a; uint32_t b; }` is trivially copyable, so
  C++ would happily blit it — with three bytes of padding, in this compiler's
  field order and this target's byte order. The receiver would have to
  reproduce all three accidents. A comment about this protects only the people
  who read comments.
- **`bool`.** `true` is permitted to be any non-zero bit pattern, so its object
  representation is the compiler's private business. Append
  `uint8_t{flag ? 1u : 0u}`.
- **Pointers and member pointers**, which fall out of the concept for free:
  neither is arithmetic nor an enumeration, and neither means anything at the
  other end of a link.

`append_native(span)` adds no length prefix. A caller who needs one appends it,
which keeps the protocol's framing visible in the protocol's own code:

```cpp
if (!msg.append_native<uint16_t>(count) || !msg.append_native(values)) {
    return;   // every append result has to be acted on
}
```

If raw object representation is ever genuinely required, it belongs behind a
deliberately alarming name — `append_object_representation()` — so that nobody
mistakes it for ordinary wire serialization.

#### 8.3.1.2 `detail::NativeScalar` is not a stable-wire promise

The concept keeps out the types with no sane representation. It does NOT, and
cannot, guarantee that what it lets through means the same thing on both ends
of a link. `append_native<T>` is a **native-representation serializer**, and the
protocol's stability is the protocol author's job. Three ways to lose, all
measured rather than imagined:

```text
enum PlainEnum { A, B };
  sizeof == 4 normally, == 1 under -fshort-enums

size_t / long / long double
  8 / 4 / 16 bytes on x86-64 mingw, quite different on Cortex-M

float / double
  IEEE-754 on every target this library targets, but that is a fact about
  those targets, not a language guarantee
```

So the rule for anything that crosses a link:

> Protocol definitions use fixed-width types — `uint8_t` … `uint64_t`,
> `int8_t` … `int64_t`. An enumeration on the wire declares its underlying
> type explicitly (`enum class Op : uint16_t`). `size_t`, `long`,
> `long double` and plain unsized enums are for local code, never for a frame.
> `float`/`double` only where both ends have agreed on the format.

One case is enforced rather than documented, because it silently defeated a
rule this layer does state: `enum class Flag : bool` passed the enumeration
branch and put a `bool` on the wire after all. An enumeration whose underlying
type is `bool` is now rejected. Byte order is untouched by any of this — see
above.

### 8.3.2 Pointer invalidation, and why it is invisible

> Any operation that increases `capacity()` may invalidate every pointer,
> reference and span into the payload.

The ordinary vector rule. What makes it harmless here is that **the public API
hands out no writable payload span at all**: a message is built through the
serializers, and there is no pointer for a caller to be holding when a growth
happens. The hazard exists in the implementation and is confined to it.

### 8.3.3 Encoding geometry

The payload is physically at `cobs::codec::raw_offset(H+C) + H` for the current
capacity `C`, and moving it at encode time would end the zero-copy story, so
encoding a payload of size `S` — a decoded region of `H + S` bytes — begins
further into the block:

```text
allocated for C
|<-- unused -->|<-- R(H+S) -->|<- H ->|<------ S ------>|
^              ^
block          encoding begins here, and so does the wire frame
```

It fits exactly: `R(H+S) <= R(H+C)` because `S <= C`, and the encoded region
ends at `cobs::codec::raw_offset(H+C) + H + S`, at most the end of the block. So a wire
frame need not start at `block[0]`; the transport is handed the span returned
by the private coordinator encoding step, while the ALLOCATION remains the
whole block and is returned as such.
Encoding copies nothing, whatever growth history the message had.

The declared length is written into `frame_raw` in the last moment it is still
plain bytes — immediately before the in-place encode. On a RETRY it is not
rewritten: COBS has already overwritten those bytes, so the stored wire span is
the only truth about that frame, which is exactly what makes a failed transport
start retryable byte-for-byte.

In terms of the granted capacity `C` and the header width `H`:

```text
wire_capacity = cobs::codec::max_wire_size(H + C)
              = cobs::codec::max_encoded_size(H + C) + 1

raw_offset    = wire_capacity - (H + C)
              = ceil((H + C) / 254) + 1               (for H + C > 0)

block size    = wire_capacity
```

The block is exactly `wire_capacity` bytes: the worst-case encoded frame for
`H + C` decoded bytes plus its delimiter fills it precisely. Every one of these
follows from the capacity storage granted, never from `tx_max_size` — that
is what makes a short message cheap.

One consequence worth stating for tests: the header shifts every COBS block
boundary by `H`, so the interesting payload lengths are the ones that put
`H + S` on a boundary — 252 and 253 for a two-byte header, not 253 and 254.

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

## 9. Storage contract

Memory is a checked compile-time strategy: one type, one template parameter,
arbitrarily many implementations. `cobs::Endpoint` never learns whether bytes come from
a heap, a static pool, external SDRAM, a TLSF arena or debug storage that
poisons released blocks. It acquires and releases RX/TX blocks through one
`cobs::Storage` contract. There are no virtuals and no runtime dispatch.

One storage type serves both directions. A custom implementation therefore
states shared placement/configuration once while retaining independent RX and
TX quotas.

### 9.1 The contract

One protocol type, exactly `cobs::RxBlock<Storage>`, and four memory operations.
Directional limits live only in `Format`.

```cpp
struct SomeStorage {
    using Format = cobs::Format<1024, 64>;
    using RxBlock = cobs::RxBlock<SomeStorage>;

    [[nodiscard]] RxBlock* acquire_rx(std::size_t requested_size) noexcept;
    void release_rx(RxBlock* block) noexcept;

    [[nodiscard]] cobs::TxBlock acquire_tx(std::size_t requested_capacity) noexcept;
    void release_tx(cobs::TxBlock block) noexcept;
};

namespace cobs {

struct TxBlock {
    std::byte*  memory   = nullptr;
    std::size_t capacity = 0;   // payload bytes, not physical bytes
};

} // namespace cobs

static_assert(cobs::Storage<SomeStorage>);
```

Obligations on a non-null TX allocation:

```text
requested <= capacity <= Format::max_send_size
the block holds at least cobs::codec::max_wire_size(length_size + capacity) bytes
the exact returned TxBlock is later passed to release_tx()
```

The TX obligation is **header-inclusive**: what gets encoded is
`[length][payload]`, so a block sized for the payload alone is one or two
bytes short and the coordinator's encoding step runs off the end of it. Use
`cobs::Format::tx_storage_size_for_capacity(capacity)` rather than
open-coding it — the shared contract test does, precisely so that storage
written against the old formula fails there instead of in the field.

Two obligations that go without saying until somebody's storage does not honour
them, and which the shared contract test therefore checks:

```text
successful live blocks remain valid, distinct and exclusively the caller's
until they are released
releasing a null RX block or empty TxBlock is a no-op, not an abuse
constructing storage is noexcept
requested_size <= Format::max_receive_size, and a non-null RX block has storage for
    exactly that many payload bytes
```

`release_rx` stays pointer-only. A second descriptor is not added merely for
symmetry: RX has nothing the caller could not already know, and segregated RX
storage can keep its size class in the typed block header or recover it from
the pointer as a private implementation detail.

The last one is easy to miss because it is not one of the four operations.
`cobs::Endpoint`'s constructors are unconditionally `noexcept` and construct storage in
place, so a storage constructor that throws reaches `std::terminate` rather
than the caller. That is consistent with a layer built entirely on `noexcept`
plus explicit failure returns — there is nowhere for an exception to go — but
it has to be written down, because a storage author reading only the four
function signatures would not guess it.

Nothing else is in the contract — no payload span, no block count, no
alignment, and above all **no physical block size**. Every one of those is a
detail of a particular strategy.

#### 9.1.0 Why `cobs::TxBlock` came back

An earlier revision of this section removed the TX descriptor deliberately,
and that was right at the time: `acquire_tx` was asked for exactly the block
one message needed, so `requested` and usable capacity were the same number
and the struct only restated what the caller already knew. A descriptor that
reports what storage *happened* to hand over physically is noise, and
worse, it invites the caller to use a number that can disagree with the
declared limit.

What changed is that the two numbers are now deliberately different:

```text
tx_max_size    the protocol limit, e.g. 1024
requested      what the container asked for this time, e.g. 100
capacity       what THIS allocation permits, e.g. 100, or 1024
```

The capacity is a fact the container cannot derive, because it depends on the
storage strategy:

```text
cobs::Heap          request 100  ->  capacity 100    (exact)
cobs::Pool          request 100  ->  capacity 1024   (the slab was paid for)
segregated storage  request 100  ->  capacity 128    (its matching class)
```

Without it, a pool that physically holds `tx_max_size` would reallocate and
copy on every growth while sitting on a kilobyte of already-committed RAM. With
it, a message on that storage is born with all the capacity it will ever need
and the growth path is simply never taken. So the descriptor is back, but it
carries a different kind of information — *how much payload this caller may
use* rather than *how many bytes exist* — and it is expressed in payload units,
so alignment and padding remain invisible.

#### 9.1.0.1 The container decides how much to ask for; storage decides what to give

These are two separate jobs and the contract keeps them separate:

```text
cobs::Message      knows its current capacity and what it now needs
             -> computes the geometric target (§8.3.1) and asks once

Storage      knows how it carves memory
             -> answers with a block and the capacity that block permits
```

`cobs::Heap` therefore has **no growth rule at all**: it allocates exactly
what was requested and reports exactly that. An earlier draft had it round up
to a power of two, which was a second growth rule living one layer below the
first — two heuristics compounding into a sequence neither of them described.
Removing it costs nothing, because the container was already computing a
geometric target before it asked.

That division is also what lets storage with completely different strategies —
exact, single-slab, segregated — share one `cobs::Message` with no `if constexpr`
anywhere. It is checked directly: the test suite runs custom storage whose rule
is `2n + 1`, and the container does not notice.

### 9.1.1 Both directions are size-aware

The asymmetry is deliberate, and it follows from what is known at the moment
of allocation:

```text
TX   the container knows the capacity it needs RIGHT NOW — at creation, and
     again at every growth
     -> ask for exactly that, and ask again if the message outgrows it
RX   the frame declares its body length in the header, which arrives before
     the body does
     -> ask for exactly that, once
```

Note what this does NOT claim: that the application knows the payload size up
front. It often does not, which is why `cobs::Message` is a container that grows
(§8.3.1). What remains asymmetric is only the number of attempts — TX can ask
again because nothing has reached the wire yet, while RX gets one chance, and
the length prefix is what makes that one chance exact instead of worst-case.

**Both directions are now size-aware, and for the same reason.** TX knows the
capacity it needs because nothing has reached the wire yet; RX knows the body
length because the frame declares it in the length prefix (§3) before a single
body byte arrives. The old asymmetry — RX committing to `rx_max_size` because
the size was unknowable — was a consequence of having no header, and the header
exists to remove it.

An earlier revision argued that making RX size-aware would mean buffering the
encoded frame, computing the decoded length, allocating, and then decoding a
second time. That is true of a format WITHOUT a length prefix, and it is why
the prefix was added rather than the second pass. With it, the decoder writes
the header into a one- or two-byte local buffer, `cobs::detail::Receiver` reads the declared
length, allocates exactly that, and the body decodes straight into its final
home — one pass, no staging buffer, no copy.

With `cobs::Heap`, a 20-byte frame therefore costs 20 payload bytes, and a
seven-byte TX capacity REQUEST costs ten (`cobs::codec::max_wire_size(1 + 7)`, or
eleven with a two-byte header) rather than the worst case of the largest frame
allowed. A single-slab pool still spends a whole block in both directions,
which is `cobs::Pool` being deterministic — exactly what an STM32 target
wants.

`release_tx` takes the whole `TxBlock` back. The capacity stays beside its
pointer for the same reason sized deletion exists: the caller knows it, so
segregated storage need not search its pools for the pointer's owner. It is the
**capacity storage reported for THAT block** — which growth may have changed
since message creation — not the logical size or encoded frame length. A
strategy is free to ignore it; `cobs::Heap` does, since sized `operator delete` is an ABI- and
runtime-dependent optimisation whose value belongs in a benchmark rather than
in a contract argument.

A block sized for capacity `C` is `cobs::codec::max_wire_size(length_size + C)` bytes:
the tight worst case for that decoded length, not the exact encoding of those
particular bytes.
Knowing the latter would need a pre-scan of the payload, i.e. another pass. Not
worth it.

### 9.1.2 Storage is taken at its word

On RX, `cobs::Endpoint` never asks how much memory it actually got. The exchange is:

> Your `Format` declares `max_receive_size = 1024`. If `acquire_rx(n)` returns
> non-null with `n <= 1024`, you are **obliged** to have given valid storage for a packet
> header plus `n` payload bytes. If you cannot, return null.

`std::allocator<T>::allocate(n)` works the same way: it hands back a pointer
or fails, never a pointer paired with "how many I really managed". So the RX
size is derived, never queried:

```cpp
packet->writable_payload(n)                     // exactly the n it was allocated for
```

The TX capacity report (§9.1.0) is not an exception to this, and it is worth
being precise about why. It is not storage confessing how much it
managed to supply — that number could disagree with `tx_max_size`, and a
contract with two disagreeing truths is exactly what this rule exists to
prevent. It is a **grant**, bounded on both sides by things already agreed:

```text
requested <= capacity <= tx_max_size
```

The caller named the floor, Format named the ceiling, and the ceiling is
the declared limit. There is one direction and no disagreement possible.

A pool that physically rounds a 1024-byte packet up to 1040, or a TX block to
the next alignment boundary, keeps that entirely to itself. The spare bytes
are nobody's business.

**An RX allocation is always ONE contiguous region, `[cobs::RxBlock][payload]`.**
An earlier draft of this section allowed storage to allocate the header and
the payload separately. It should not have: `cobs::RxBlock` locates its payload as
`this + sizeof(cobs::RxBlock)`, so a split allocation is not merely discouraged but
impossible — it would need a second pointer in every packet, paid for by every
packet, to serve storage that wants to scatter one across two pieces of RAM.
`cobs::Heap` has no difficulty honouring contiguity:

```cpp
void* memory = ::operator new(sizeof(RxBlock) + requested_size, std::nothrow);
RxBlock* block = std::construct_at(static_cast<RxBlock*>(memory));
```

so heap and pool end up with identical geometry and differ only in where the
region came from.

`cobs::detail::Receiver` passes `writable_payload()` the same number it allocated with, which
is also the declared body length — so the decoder is given a segment that is
exactly the frame's size. That is load-bearing twice over: on an exact heap
allocation a longer span would run off the end of the block, and with any storage
a longer span would swallow an over-length frame instead of rejecting it. A
storage that declares more than it can supply is simply broken, and the place
to catch that is inside the strategy, at compile time.

### 9.1.3 Obligations

Exhaustion is a null return, never an error code: `if (packet == nullptr)` is
the check either way.

**Storage never touches the RX block's fields.** `refs`, `size`, `next_ready`
and `owner` are private to the RX vertical, and `owner` in particular is set by
`cobs::detail::Receiver`, not by storage. An earlier revision had both built-in strategies
stamp it themselves, which made it a hidden FIFTH obligation — absent from the
signatures, absent from this list, and unverifiable by the contract test once
the field became private. Storage written to the letter of §9 therefore
returned a block whose owner was null, and the first `cobs::Packet` release
dereferenced it. The suite includes minimal custom storage with exactly one
`Format`, one `RxBlock`, and four operations so this cannot return.

So storage's whole job on RX is: produce memory for
`sizeof(RxBlock) + requested_size`, construct the block in it, and hand it
back; later, destroy and reclaim. Ownership is established one layer up.

Three further obligations that are easy to get wrong:

- **`release_rx` runs the block's destructor.** Storage owns that, because
  only it knows whether the pointer is valid at all. A
  validating pool must refuse a foreign or already-freed pointer **before**
  running any destructor on it — tearing down an object on memory that may
  belong to somebody else is worse than the leak a refusal costs.
- **RX and TX quotas are independent.** A strategy may share one backing store,
  but RX exhaustion must never starve TX. A link that cannot transmit because
  the application is holding received packets is a deadlock, not back-pressure.
- **A rejected release leaks one block; it must never corrupt
  storage.** Losing a block is recoverable and countable. A corrupted free
  list is neither.

### 9.1.4 Block counts are not part of the contract

`cobs::Endpoint` never asks how many blocks exist, so the generic contract does not
mention them. How much memory a strategy is willing to hand out, and by what
scheme, is entirely its own business:

```text
cobs::Heap     no counts at all — whatever the heap allows
cobs::Pool    RxBlockCount and TxBlockCount, its own parameters
some SDRAM storage    a bitmap, a TLSF arena, or something stranger
```

Keeping counts out is what lets those live side by side under one contract.

That said, storage that HAS counts has to be sized, and this is the rule for
`cobs::Pool`:

> `TxBlocks` must cover the maximum number of TX blocks simultaneously owned
> anywhere in the pipeline: non-empty `cobs::Message` objects held by the application
> plus the one block that may be held by `cobs::Endpoint::activeTx`. If the application
> keeps a pending queue filled while one frame is in flight, this is
> `pending_depth + 1`. If sending removes the head and the queue is not
> replenished until that transfer completes, the active block merely replaces
> the former head and no extra block is required.

The second case matters because "queue depth plus one" over-states the need:
ownership of the same block simply moves from the queued `cobs::Message` to
`cobs::Endpoint::activeTx`.

> An empty result from `make_message()` is the back-pressure signal that the
> configured TX storage has been exhausted; callers must handle it.

It is not a silent failure — `make_message()` returns an honest empty message — but
an upper layer is perfectly capable of ignoring the result and inventing its
own mystery.

### 9.2 Protocol and memory each have one source of truth

`Format` owns protocol geometry. Storage owns only memory strategy and
quotas, and names the format it serves:

```text
cobs::Format<Rx, Tx>
├── max_receive_size     largest BODY this instance accepts
├── max_send_size        largest BODY this instance can send
├── length_size / LengthType
└── checked framing geometry

Storage
├── using Format = ...
├── RX strategy / quota
├── TX strategy / quota
├── acquire_rx(n)  / release_rx()
└── acquire_tx()   / release_tx()
```

`cobs::Endpoint` reads those numbers from `StorageT::Format` and never reconstructs
them. It republishes them under `max_receive_size` and `max_send_size` (§4.1).

The default is **`cobs::Heap`**, as `UART_COBS_ARCHITECTURE.md` §1 has
said from the start, which makes the common spelling `cobs::Endpoint<>`. It is
parameterized rather than unbounded, because "no limit" is not available to
us: the length field is itself fixed-width, so the largest frame the format can
describe has to be decided when the type is instantiated. The PER-FRAME
allocation is exact (§6.1.1); `Format::max_receive_size` is the ceiling above
which a declared length is refused.

```cpp
namespace cobs {

template<class Format = cobs::Format<1024, 1024>>
class Heap;

template<class StorageT = cobs::Heap<>>
class Endpoint;

} // namespace cobs
```

so `cobs::Endpoint<>` gets workable defaults and a bigger heap-backed link is still one
line:

```cpp
using Wire = cobs::Format<4096, 512>;
cobs::Endpoint<cobs::Heap<Wire>> cobs;
```
 A fixed pool
default would look more embedded-minded and be worse: it would force the
library to invent a block count on the user's behalf, and every `cobs::Endpoint` object
would then carry that fixed quota whatever its workload. A target where
`malloc` is unwelcome is exactly a target whose author chooses storage
deliberately:

```cpp
using Wire = cobs::Format<1024, 1024>;
using Memory = cobs::Pool<Wire, /* RX blocks */ 8, /* TX blocks */ 2>;
cobs::Endpoint<Memory> cobs;
```

RX and TX limits are separate on purpose. A device that receives 1 KB commands
and replies with 64-byte acknowledgements should be able to say so, rather
than paying for the larger number twice.

### 9.3 What storage must not contain

```text
cobs::Endpoint<StorageT>
        |                    |
     protocol             memory
        |                    |
  decoder / encoder     acquire / release
  RX and TX state       strategy and quotas
  ready queue           anything: heap, pool, SDRAM, external region
  cobs::Packet / cobs::Message
```

A storage strategy that starts to know about encoding, decoder state, the ready
queue or `cobs::Packet` behaviour has stopped being storage. Keeping it focused is
what makes "write your own storage" a small job rather than a diploma in
metaprogramming.

### 9.4 `cobs::Endpoint` owns storage by value

```cpp
namespace cobs {

template<class StorageT = cobs::Heap<>>
class Endpoint final {
    [[no_unique_address]] StorageT m_storage{};
};

} // namespace cobs
```

By value, so that `cobs::Endpoint<> cobs;` works with no ceremony, a stateless heap
strategy costs nothing (`[[no_unique_address]]`), and a `cobs::Pool` can
simply contain its RX and TX pools.

**`cobs::Endpoint` is therefore neither copyable nor movable.** That is not tidiness:
`cobs::Packet` holds a pointer to storage living inside the `cobs::Endpoint` object,
so moving one would turn every outstanding packet's owner pointer into a
souvenir of a previous life.

Storage needing runtime arguments is constructed in place, so it
need not be copyable or movable at all:

```cpp
cobs::Endpoint<MyStorage> cobs{std::in_place, region_base, region_size};
```

There is deliberately no constructor taking a ready-made storage object. It
would only serve copyable ones, and one way in is enough.

This also corrects an argument made earlier in review. It is **not** true that
a fixed strategy forces the whole `cobs::Endpoint` object into DMA-accessible RAM. That
follows only if storage keeps its TX blocks inside itself; storage holding
pointers or spans into external RX/TX regions leaves `cobs::Endpoint` placeable
anywhere, and only the **TX region** must be DMA-visible — the transport reads
it directly, unlike the RX pool, which only the CPU ever writes.

### 9.5 Transport is not part of it

The transport is bound separately (§2.1) and never travels through the
storage: memory and byte movement are unrelated concerns, and coupling them
would mean new storage for every transport.

---

## 10. Counters

Counters only — no hot-path instrumentation unless a probe is compiled in,
following the same rule as the transport layer.

```text
rx: frames_delivered, frames_lost, allocation_failure,
    malformed, oversize, length_mismatch, resyncs
tx: frames_sent, send_refused_busy, send_failed
```

Each RX counter answers a different question, and `length_mismatch` exists so
that none of them has to answer two:

```text
malformed          a structural COBS error — a delimiter inside a data block
oversize           a declared length above rx_max_size
length_mismatch    a header that is absent or truncated, or a body whose
                   actual length disagrees with what the frame declared
allocation_failure storage refused an RX block AFTER a valid length
frames_lost        every frame that did not reach the queue, whatever the cause
resyncs            only the failures found BEFORE the delimiter, which leave
                   the rest of a frame in the stream to be skipped
```

`frames_lost` is the single place a transport gap becomes visible to the
application, and it is deliberately a number rather than an event (§7).

`resyncs` is the one that repays attention. A failure the DELIMITER revealed
is already synchronized, so counting a resync there would mean throwing away
the next perfectly good frame:

```text
no header, truncated header, body shorter than declared   no resync
empty-packet allocation refused (it happens after the
    delimiter, since a zero-length body needs no segment) no resync

body longer than declared, declared oversize, allocation
    refused for a non-empty body, transport gap           resync
```

---

## 11. Test plan

The decoder is a non-template class over a plain span, so all of this runs as
a host binary with no HAL, no pool and no transport.

### 11.1 Lengths

```text
0, 1, 253, 254, 255, 508, 509, rx_max_size (RX) / tx_max_size (TX)
```

and, because the length header shifts every COBS block boundary by
`length_size`, the lengths that put `length_size + n` on those boundaries as
well — 252 and 253 for a two-byte header, not only 253 and 254.

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
and `encode` output length `<= cobs::codec::max_encoded_size(len)`, with the
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

- CRC / integrity trailer. Reserved by the definition of the declared length
  (§3): it counts the whole BODY, so a trailer is already inside the number
  every frame carries. What changes when it arrives is how much of the body
  the application may use — `rx_max_size - CRC` instead of `rx_max_size` —
  and nothing else. The decoder, the block layout and the size arithmetic all
  stay as they are.
- Atomic refcount policy for cross-task packet sharing (§6.4).
- Any TX queue. Busy policy belongs to the layer above, exactly as it does
  for the byte transport.
