<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# COBS wire protocol

This document is the normative wire contract for the current COBS engine.
It is independent of UART, heap versus pool storage, transport delegates, and
application scheduling. Two peers interoperate only when this contract and
their configured limits are compatible.

## 1. Terms

| Term | Meaning |
|---|---|
| body | bytes counted by the engine length field; in v1, the application payload |
| payload | application-visible bytes; equal to the body in v1 |
| decoded frame | fixed-width length field followed by the body |
| encoded frame | canonical COBS encoding of the decoded frame, without delimiter |
| wire frame | encoded frame followed by one `0x00` delimiter |
| `H` | length-field width in bytes |
| `S` | actual payload/body size for one frame |
| `C` | granted TX payload capacity for one block, `S <= C` |

Future integrity bytes, if added, belong inside the body and are included in
the declared length. They are not part of this version.

## 2. Frame grammar

```text
wire_frame    ::= COBS(decoded_frame) 0x00
decoded_frame ::= length body
length        ::= H bytes, unsigned little-endian
body          ::= exactly the number of bytes declared by length
```

No leading delimiter is required. Leading or repeated bare delimiters are
harmless synchronization bytes and deliver no packet.

The encoder emits canonical/minimal COBS:

- `0x00` appears only as the final delimiter;
- every non-`0xFF` code block represents an implicit zero if another block
  follows;
- a final full `0xFF` block is not followed by a redundant `0x01` block;
- a payload ending in zero does retain the final `0x01` needed to materialize
  that zero.

The decoder accepts structurally valid non-canonical COBS as well. Canonical
encoding is a sender requirement, not a receiver rejection rule.

## 3. Length field and directional limits

Each `cobs::Format<RxMax, TxMax>` declares two body limits. Its concise forms
are deliberately symmetric:

```cpp
cobs::Format<>       // RX 255, TX 255
cobs::Format<1024>   // RX 1024, TX 1024
cobs::Format<1024, 64>
```

```text
max_receive_size = largest body this instance accepts
max_send_size    = largest body this instance can build
```

The decoded frame is `H` bytes longer than either body. The larger
directional limit chooses one width for both directions:

```text
wire_length_limit = max(max_receive_size, max_send_size)
H = length_size   = wire_length_limit <= 255 ? 1 : 2
```

Both limits must fit `uint16_t`; no wider v1 length field exists.

Length bytes are explicitly little-endian:

```text
H = 1:  length[0] = S
H = 2:  length[0] = S & 0xff
        length[1] = (S >> 8) & 0xff
```

The length counts only body bytes, never itself.

### 3.1 Compatibility conditions

Three different checks must not be conflated:

| Check | Condition | Consequence |
|---|---|---|
| wire compatibility | `A.length_size == B.length_size` | required for any frame to be interpreted consistently |
| per-frame acceptance | `declared <= receiver.max_receive_size` | decides whether this frame can be delivered |
| full-range compatibility | `A.max_send_size <= B.max_receive_size` and the reverse | guarantees every legal sender frame is accepted by its peer |

Only the first is unconditional. Peers may use asymmetric limits:

```text
Peer A: Format<1024, 64> -> H = 2
Peer B: Format<64, 1024> -> H = 2
```

These peers share a wire format and have full-range compatibility in both
directions. Replacing storage without changing `Format` cannot alter their
wire bytes.

Integrations should lock the width at compile time:

```cpp
using Link = cobs::Endpoint<MyStorage>;
static_assert(Link::length_size == 2);
```

## 4. Empty input, empty packet, and bare delimiter

These are three different cases.

| Bytes | Pure COBS meaning | Engine meaning |
|---|---|---|
| no bytes | no event | no event |
| `00` | bare delimiter | synchronization/no-op |
| `01 00` | valid empty decoded COBS frame | invalid engine frame: length field absent |
| COBS encoding of `H` zero bytes + `00` | decoded length is zero | valid empty application packet |

For a one-byte length field, the canonical empty engine packet is:

```text
decoded: 00
wire:    01 01 00
```

For a two-byte length field:

```text
decoded: 00 00
wire:    01 01 01 00
```

`01 00` is counted as `length_mismatch` rather than `malformed`. Its COBS
structure is valid; it fails only the engine protocol layered above COBS.

## 5. Worked one-byte-header vectors

The following vectors assume `Format` chooses `H = 1`:

| Application payload | Decoded `[length][body]` | Canonical wire |
|---|---|---|
| empty | `00` | `01 01 00` |
| `11` | `01 11` | `03 01 11 00` |
| `00` | `01 00` | `02 01 01 00` |
| `11 22` | `02 11 22` | `04 02 11 22 00` |
| `11 00 22` | `03 11 00 22` | `03 03 11 02 22 00` |

For `H = 2`, the one-byte body `11` is:

```text
decoded: 01 00 11
wire:    02 01 02 11 00
```

These examples include the engine length field. A pure COBS test vector that
encodes only application bytes is not an engine frame.

## 6. Size arithmetic

Let `N` be a decoded byte count. The codec exposes:

```cpp
max_encoded_size(0) = 1
max_encoded_size(N) = N + ceil(N / 254), N > 0
max_wire_size(N)    = max_encoded_size(N) + 1
raw_offset(N)       = max_wire_size(N) - N
```

`max_encoded_size` excludes the delimiter. `max_wire_size` includes it.
The encoded-size expression is a tight upper bound, not the exact length for
arbitrary data. Zero-free input attains it for every `N`.

The exact encoded length is:

```text
N
+ number of emitted 0xFF code blocks
+ 1 when the final block is not a 0xFF block
```

For `N <= 254`, encoded size is always `N + 1`, independent of zero
placement. Selected pure-codec values:

| decoded bytes `N` | maximum encoded | maximum wire |
|---:|---:|---:|
| 0 | 1 | 2 |
| 1 | 2 | 3 |
| 3 | 4 | 5 |
| 254 | 255 | 256 |
| 255 | 257 | 258 |
| 508 | 510 | 511 |
| 509 | 512 | 513 |

For an application payload of size `S`:

```text
decoded_size = H + S
maximum wire = max_wire_size(H + S)
```

For a TX block granting payload capacity `C`:

```text
physical block size = max_wire_size(H + C)
payload offset      = raw_offset(H + C) + H
```

`Format` statically checks that its header-inclusive size arithmetic cannot
overflow `size_t`.

## 7. Streaming decoder contract

`cobs::codec::Decoder` is a non-template state machine with no allocator,
transport, format limit, or packet ownership.

### 7.1 States

```text
Synced              waiting for the first code byte
Decoding            inside a frame
DropUntilDelimiter  discarding after lost framing continuity
```

In `Synced`, a delimiter is consumed as a no-op. A non-zero byte begins a
COBS block.

### 7.2 Events

`consume(input)` stops at the first event requiring owner action:

| Event | Meaning |
|---|---|
| `None` | input ended without an owner-visible event |
| `NeedOutput` | attach the next output segment or enter discard mode |
| `FrameComplete` | delimiter ended a structurally valid COBS frame |
| `Malformed` | delimiter appeared while data bytes were still owed |

`Result::consumed` says how much input was taken. On `NeedOutput` it may be
zero: the encoded byte needing a destination is intentionally left for the
next call. `Result::decoded_size` is meaningful on `FrameComplete` and totals
all attached output segments.

`NeedOutput` can occur multiple times per frame. An owner may call
`prepare_output()` while the decoder is `Synced` to prepare a known first
segment; the endpoint does this with its fixed length-field buffer and avoids
one event/call round trip per normal frame. Without preparation, the first
`NeedOutput` asks for that segment. Later requests ask for the next segment;
the endpoint uses exactly the declared body allocation. Attaching a new
segment while the current decoding segment still has room is ignored.
Attaching an empty segment does not mean failure and can cause repeated
`NeedOutput`; refusal is expressed with `discard_until_delimiter()`.

### 7.3 Implicit zero

A code other than `0xFF` owes an implicit zero only if another block follows:

```text
block ends, code != FF -> pending zero
next byte is code      -> output zero, then process code
next byte is 00        -> cancel pending zero and complete frame
code == FF             -> no pending zero
```

This delayed decision is what makes decoding independent of transport chunk
boundaries.

### 7.4 Structural malformed frame

```text
block bytes remaining == 0 and byte == 00 -> complete
block bytes remaining >  0 and byte == 00 -> malformed
```

The delimiter that reveals a malformed block also restores synchronization.
The decoder reports `Malformed` and returns to `Synced` immediately; it must
not discard through a second delimiter.

There is deliberately no decoder `Oversize` event. Only the receiver knows
`max_receive_size` and the engine length field.

## 8. Receiver validation and resynchronization

The receiver decodes the header first, then makes one of the following exact
decisions:

| Condition | Primary counter | Frame lost | Enter discard-until-delimiter |
|---|---|---:|---:|
| declared equals actual | delivered | no | no |
| no length field | length mismatch | yes | no |
| truncated two-byte length | length mismatch | yes | no |
| declared zero, body follows | length mismatch | yes | yes |
| declared above limit, body follows | oversize | yes | yes |
| declared above limit, delimiter follows header | oversize | yes | no |
| actual body shorter than declared | length mismatch | yes | no |
| actual body longer than declared | length mismatch | yes | yes |
| non-empty RX allocation refused | allocation failure | yes | yes |
| empty RX allocation refused | allocation failure | yes | no |
| delimiter inside a COBS block | malformed | yes | no |
| transport gap | gap/lost-frame accounting | yes | yes |

The rule is temporal:

- a failure discovered before the delimiter leaves unknown tail bytes and
  requires a scan to the next delimiter;
- a failure exposed at the delimiter is already synchronized and must not
  sacrifice the following valid frame.

`frames_lost` counts every frame that does not reach the ready queue.
`resyncs` counts only transitions that really hunt for a later delimiter.

## 9. Transport gaps

`Endpoint::notify_gap()` means bytes were physically lost and framing
continuity is no longer knowable. It:

1. releases any block currently being built;
2. increments `frames_lost` and `resyncs`;
3. resets receiver frame-local state;
4. places the decoder in `DropUntilDelimiter`;
5. leaves already-ready packets untouched.

This happens even if local state looked “between frames”. After physical loss,
the next observed byte might be the middle of a frame. The first later
delimiter restores synchronization and is consumed without delivering a
packet.

No gap event is emitted to the application. Its observable effect is the
missing packet and the RX statistics snapshot.

## 10. In-place TX geometry

For a block granting `C` payload bytes:

```text
| encoder headroom for H+C | H-byte length | C-byte payload area |
^                           ^               ^
block start                 raw frame       public builder data
```

The payload is physically placed using capacity geometry:

```text
payload = block + raw_offset(H + C) + H
```

When only `S <= C` bytes were appended, encoding does not move them. It begins
later in the same allocation using actual-size geometry:

```text
decoded = H + S
encoding begin = payload - H - raw_offset(decoded)
wire span capacity = max_wire_size(decoded)
```

The length is written immediately before encoding, then `encode_in_place()`
reads raw bytes forward and writes encoded bytes forward.

### 10.1 Overlap invariant

While unread raw data remains:

```text
written <= consumed + raw_offset(N) - 1
```

Equivalently, the next write remains strictly before the next unread byte.
For a zero-free decoded region of `N >= 1` bytes, the maximum lead of writes
over reads is exactly:

```text
ceil(N / 254) = raw_offset(N) - 1
```

The bound is tight. The additional one byte in `raw_offset` is load-bearing;
removing it allows the writer to collide with the next unread byte. Any change
that emits another code byte must revisit the proof in `COBS_ENGINE.md` and
the sentinel/property tests.

### 10.2 Retry identity

Once a message is encoded, the length field and raw payload have been
overwritten in place. If the transport refuses to start:

- the message remains Encoded;
- no header is rewritten;
- no second encoding occurs;
- the next send attempt presents the same pointer range and byte-identical
  wire frame.

Only after a sender accepts the span does ownership move to the endpoint.

## 11. Wire-change checklist

A protocol-affecting change is incomplete unless all of the following are
reviewed together:

1. `Format` remains the single source of limits and `H`.
2. Both peers agree on length width and byte order.
3. Body-length semantics, including any trailer, are explicit.
4. `max_encoded_size`, `max_wire_size`, and `raw_offset` remain consistent.
5. The in-place overlap proof still holds.
6. Empty packet and bare-delimiter behavior are preserved or versioned.
7. Every validation failure has one counter and correct resync timing.
8. Heap, Pool, and reference encoder fixtures produce identical bytes.
9. Boundaries around 252/253/254/255 and 508/509 include the header width.
10. Whole-frame, byte-at-a-time, code-boundary, and delimiter-boundary
    streaming tests pass.
