<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# COBS wire protocol v2

This is the normative current engine contract, independent of storage and
transport. The low-level COBS codec itself is unchanged. Version numbers are
documentation labels: **no version byte, CRC negotiation, autodetection or
fallback is present on the wire**.

## 1. Terms

| Term | Meaning |
|---|---|
| payload | application-visible bytes supplied to Message and exposed by Packet |
| trailer | exactly `Crc::wire_size` bytes serialized by the selected CRC policy |
| body | payload + trailer; the length field counts this |
| decoded frame | little-endian length field + body |
| wire frame | COBS encoding of the decoded frame + final 00 |
| H / W | length-field width / trailer width |
| S / C | actual useful payload size / useful TX capacity, S <= C |

## 2. Grammar and integrity

```text
wire    = COBS(length_le | payload | trailer) | 00
length  = S + W                         (does not count itself)
trailer = policy.store(policy.calculate(payload))
```

CRC covers payload only, **not the length field**. RX separately requires the
actual body length to equal the declared length and to contain the trailer.
A CRC does not authenticate the peer and is not a cryptographic MAC.

The default policy is CRC-16/MODBUS: init FFFF, reflected polynomial A001,
no final XOR, two trailer bytes low byte first. Table and Bitwise produce the
same values and wire order. A custom policy controls its own calculation,
result type, width and codec; the library does not validate its algorithm.
Both peers must agree on those semantics.

No leading delimiter is required. Bare/repeated delimiters deliver no packet.
The encoder emits minimal canonical COBS (no redundant 01 after a final full
FF block); the decoder also accepts structurally valid non-canonical COBS.
A payload or trailer ending in zero keeps the code needed to materialize it.

## 3. Format and limits

```cpp
cobs::Format<>                       // CRC16, RX/TX payload 253, H=1
cobs::Format<crc::Crc16Table>         // same bytes and capacity
cobs::Format<crc::Crc16Bitwise, 1024> // explicit 1024 useful bytes, H=2
cobs::Format<crc::Crc32Table, 4096>   // 4096 useful bytes plus CRC4, H=2
cobs::Format<crc::NoCrc, 255>         // exact old v1 H1 format
cobs::Format<crc::NoCrc, 1024, 64>    // old asymmetric v1 H2 format
```

The general form is `Format<Crc, MaxReceivePayload, MaxSendPayload>`.
Omitted TX equals RX. Omitted RX is `255 - Crc::wire_size`, so the default
body fits an unsigned one-byte length. A trailer above 255 requires explicit
payload limits instead of an unsigned-wrapping default.

Explicit numeric limits always mean useful payload, never physical storage.
Both bodies must fit 65535 bytes; no 32-bit length is offered.

```text
max_receive_body = max_receive_size + W
max_send_body    = max_send_size + W
H = max(max_receive_body, max_send_body) <= 255 ? 1 : 2
```

Length is explicitly little-endian on every CPU; H2 writes the low byte then
the high byte. The selected CRC codec controls trailer order independently.
Application append_native/BE/LE calls cannot change either envelope.

### Compatibility

Peers need equal H and matching integrity semantics/codec. Their useful
limits may differ. With the same CRC, `Format<Crc, 1024, 64>` and
`Format<Crc, 64, 1024>` agree on H2 and cover each other's full transmit
range. Replacing memory or Bitwise with its equivalent Table cannot change
wire bytes.

Migration requires a coordinated configuration of both ends. An old NoCrc
receiver can accept new CRC-bearing frames and expose trailer bytes as
payload: the v2 payload `41 42` is delivered by a v1 H1 receiver as
`41 42 B1 D1`. In the reverse direction some bytes can accidentally satisfy
CRC; mismatched configurations are **not guaranteed to reject**. Keep
`Format<crc::NoCrc, 255>` explicitly when byte-for-byte legacy is required.

## 4. Empty payload and delimiter

No input causes no event. A bare `00` is synchronization only.
`01 00` is pure COBS for no decoded bytes, but lacks the engine header and
is a length_mismatch.

An empty application payload is valid. CRC is calculated over the empty
span; default CRC16 is FFFF:

```text
default H1 decoded: 02 FF FF
default H1 wire:    04 02 FF FF 00
NoCrc H1 decoded:   00
NoCrc H1 wire:      01 01 00
NoCrc H2 decoded:   00 00
NoCrc H2 wire:      01 01 01 00
```

An empty payload still owns an RX block. With nonzero W, a declared body
shorter than W is invalid and rejected without allocation.

## 5. Locked vectors

Default CRC16/H1:

| Payload | Decoded length + body | Canonical wire |
|---|---|---|
| empty | 02 FF FF | 04 02 FF FF 00 |
| 41 42 | 04 41 42 B1 D1 | 06 04 41 42 B1 D1 00 |

Explicit NoCrc/H1 (v1 regression oracle):

| Payload | Decoded length + body | Canonical wire |
|---|---|---|
| empty | 00 | 01 01 00 |
| 11 | 01 11 | 03 01 11 00 |
| 00 | 01 00 | 02 01 01 00 |
| 11 22 | 02 11 22 | 04 02 11 22 00 |
| 11 00 22 | 03 11 00 22 | 03 03 11 02 22 00 |

NoCrc/H2, payload 11: decoded `01 00 11`, wire `02 01 02 11 00`.
These are engine vectors, not pure COBS encodings of application data alone.

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
decoded_size = H + S + W
maximum wire = max_wire_size(H + S + W)
```

For a TX block granting payload capacity `C`:

```text
physical block size = max_wire_size(H + C + W)
payload offset      = raw_offset(H + C + W) + H
```

`Format` statically checks that its header-and-trailer-inclusive size arithmetic cannot
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
| declared equals actual, body >= W, integrity matches | delivered | no | no |
| declared < W, body follows | length mismatch | yes | yes |
| declared < W, delimiter follows header | length mismatch | yes | no |
| fully delimited body fails integrity | crc_errors | yes | no |
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
| encoder headroom for H+C+W | H-byte length | C-byte payload area | W-byte trailer area |
^                           ^               ^
block start                 raw frame       public builder data
```

The payload is physically placed using capacity geometry:

```text
payload = block + raw_offset(H + C + W) + H
```

When only `S <= C` bytes were appended, encoding does not move them. It begins
later in the same allocation using actual-size geometry:

```text
decoded = H + S + W
encoding begin = payload - H - raw_offset(decoded)
wire span capacity = max_wire_size(decoded)
```

The length and policy trailer are written immediately before encoding, then `encode_in_place()`
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
- no header or trailer is rewritten;
- the CRC calculator is not invoked again;
- no second encoding occurs;
- the next send attempt presents the same pointer range and byte-identical
  wire frame.

Only after a sender accepts the span does ownership move to the endpoint.

## 11. Wire-change checklist

Current regression tests include every built-in CRC width/method, arbitrary sums,
stateful policies, exact inverse geometry and legacy vectors. A protocol-affecting change is incomplete unless all of the following are
reviewed together:

1. `Format` remains the single source of limits and `H`.
2. Both peers agree on length width and integrity width/semantics/codec.
3. Body-length semantics, including any trailer, are explicit.
4. `max_encoded_size`, `max_wire_size`, and `raw_offset` remain consistent.
5. The in-place overlap proof still holds.
6. Empty packet and bare-delimiter behavior are preserved or versioned.
7. Every validation failure has one counter and correct resync timing.
8. Heap, Pool, and reference encoder fixtures produce identical bytes.
9. Boundaries around 252/253/254/255 and 508/509 include the header width.
10. Whole-frame, byte-at-a-time, code-boundary, and delimiter-boundary
    streaming tests pass.
