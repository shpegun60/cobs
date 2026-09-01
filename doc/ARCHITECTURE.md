# COBS architecture

This is the canonical entry point for the current COBS implementation.
It describes component boundaries, the supported API, ownership, lifetimes,
and the dependency direction. The exact wire contract is in `PROTOCOL.md`;
the storage extension contract is in `STORAGE.md`. `COBS_ENGINE.md` retains
the longer design rationale, state-machine traces, and proofs.

The implementation is idiomatic C++20. Transport delegates, explicit state
machines, typed ownership, and the separation between UART and COBS are
deliberate parts of the design.

## 1. Scope

The library turns an ordered stream of bytes into immutable packets and builds
packet bodies into framed byte spans for a transport:

```text
RX
byte transport -> Endpoint::consume()
               -> codec::Decoder
               -> detail::Receiver
               -> Storage::acquire_rx()
               -> Packet

TX
Endpoint::make_message()
               -> Message
               -> Storage::acquire_tx()
Endpoint::send()
               -> in-place encoder
               -> sender delegate
Endpoint::poll()
               -> busy-query delegate
               -> Storage::release_tx()
```

The byte transport can be UART, TCP, a loopback, or a test double. It is not a
template parameter and is not owned by the endpoint. COBS owns framing and
packet/block lifetime; the transport owns byte movement and its own delivery
or hardware-error reporting.

The current version intentionally has:

- one active TX transfer and no internal TX queue;
- a plain, non-atomic RX reference count;
- no CRC or integrity trailer;
- no thread synchronization;
- no HAL dependency in the COBS layer.

## 2. Public surfaces

### 2.1 Application surface

Application code includes only:

```cpp
#include "Cobs.h"
```

The stable application vocabulary is:

| Type or member | Responsibility |
|---|---|
| `cobs::Endpoint<StorageT>` | assembled RX/TX coordinator; owns storage |
| `cobs::Message<StorageT>` | move-only TX body builder and exclusive block owner |
| `cobs::Packet<StorageT>` | copyable immutable RX packet handle |
| `cobs::SendResult` | exact result of a send attempt |
| `cobs::Stats` | value snapshot of protocol counters |
| `Endpoint::bind(sender, busy)` | installs one consistent transport pair |
| `Endpoint::unbind()` | removes that pair while no TX block is active |
| `Endpoint::consume(bytes)` | feeds an arbitrary ordered byte chunk |
| `Endpoint::notify_gap()` | reports that bytes were physically lost |
| `Endpoint::pop_packet()` | transfers one ready packet reference to the caller |
| `Endpoint::make_message(hint)` | creates an empty TX builder |
| `Endpoint::send(message)` | attempts to transfer the message block |
| `Endpoint::poll()` | reclaims an accepted TX block after transport release |
| `Endpoint::stats()` | returns combined RX/TX counters by value |

The following short example is application-shaped: it names no decoder,
receiver, block, or allocation operation.

```cpp
#include "Cobs.h"

cobs::Endpoint<> endpoint;

bool configure_link(cobs::Endpoint<>& engine,
                    cobs::Endpoint<>::Sender sender,
                    cobs::Endpoint<>::BusyQuery busy)
{
    return engine.bind(static_cast<cobs::Endpoint<>::Sender&&>(sender),
                       static_cast<cobs::Endpoint<>::BusyQuery&&>(busy));
}

bool queue_command(cobs::Endpoint<>& engine,
                   uint16_t command,
                   std::span<const uint8_t> body)
{
    auto message = engine.make_message(sizeof(command) + body.size());
    if (!message ||
        !message.append_native(command) ||
        !message.append_bytes(body)) {
        return false;
    }

    return engine.send(message) == cobs::SendResult::Sent;
}
```

`append_native()` writes the native object representation of accepted scalar
types. A stable cross-platform protocol still uses fixed-width integer types
and explicitly sized enum underlying types. It does not use `size_t`, `long`,
plain enums, or structs with padding as wire fields.

### 2.2 Storage extension surface

A custom memory strategy includes:

```cpp
#include "Storage.h"
```

That header exports `cobs::Format`, `cobs::RxBlock`, `cobs::TxBlock`,
the `cobs::Storage` concept, and the built-in `cobs::Heap` and `cobs::Pool`
strategies. A storage author does not include or depend on `detail/` headers.
See `STORAGE.md` for the complete syntactic and behavioral contract.

### 2.3 Codec surface

`Codec.h` exposes the transport- and allocation-independent framing primitive:

- `cobs::codec::Decoder`;
- `max_encoded_size()`, `max_wire_size()`, and `raw_offset()`;
- `encode_in_place()`.

This is useful for protocol tests and deliberately lower-level integrations.
Normal packet applications use `Endpoint` instead.

### 2.4 Detail surface

Everything under `cobs/detail/` is implementation detail:

- `detail::Receiver`;
- `detail::Message.h` and `detail::Packet.h`, reached through `Cobs.h`;
- `detail::BlockPool`;
- `detail::NativeScalar`.

Applications must not include those headers or depend on their fields,
layouts, helper names, or coordinator-only operations. White-box tests may do
so explicitly.

## 3. Dependency direction and files

```text
Codec.h  <---------------- Decoder.cpp / Encoder.cpp
   ^
   |
Format.h
   ^
   |
Storage.h <--------------- detail/BlockPool.h
   ^  ^                         ^
   |  |                         |
   |  +---- detail/Packet.h     +---- cobs::Pool
   |        detail/Message.h
   |        detail/Receiver.h
   |                 ^
   +-----------------+
                     |
                  Cobs.h
```

| File | Current responsibility |
|---|---|
| `cobs/Codec.h` | pure COBS decoder API and in-place encoder geometry |
| `cobs/Decoder.cpp` | non-template streaming decoder implementation |
| `cobs/Encoder.cpp` | non-template canonical in-place encoder |
| `cobs/Format.h` | protocol limits, length width, byte order, checked sizes |
| `cobs/Storage.h` | storage concept, ownership blocks, Heap, Pool |
| `cobs/Stats.h` | public protocol counter snapshot |
| `cobs/detail/Receiver.h` | RX allocation, validation, queue, and ownership |
| `cobs/detail/Message.h` | TX builder, growth, encoding, exclusive ownership |
| `cobs/detail/Packet.h` | intrusive shared RX handle |
| `cobs/detail/BlockPool.h` | fixed-block memory primitive used by Pool |
| `cobs/Cobs.h` | public umbrella and assembled Endpoint |
| `cobs/cobs.pri` | reusable qmake source/header boundary |

Protocol geometry points downward into the codec; storage names one format.
The codec never points upward into storage, ownership, endpoint, or transport.
UART never enters this dependency graph.

## 4. Endpoint composition

`cobs::Endpoint<StorageT>` owns the complete instance state:

```text
Endpoint
├── [[no_unique_address]] StorageT m_storage
├── detail::Receiver<StorageT> m_rx
│   ├── codec::Decoder
│   ├── StorageT&
│   ├── 1-2 length bytes and RX stage
│   ├── one building RxBlock*
│   ├── intrusive ready-head / ready-tail
│   └── Stats::Rx
├── Transport m_transport
│   ├── owning tiny::delegate sender
│   └── owning tiny::delegate busy query
├── TxBlock m_activeTx
└── Stats::Tx m_txStats
```

Storage is owned by value. The receiver refers to that exact object, and every
message or packet released through the endpoint ultimately returns memory to
it. `Endpoint` is therefore neither copyable nor movable.

The private `Transport` groups the sender and busy-query delegates because
they describe one borrowing relationship. It adds no separate bound flag:
bound state is derived from the two delegates.

## 5. RX data and ownership flow

### 5.1 Streaming

`consume()` accepts any non-owning span shape:

- part of one frame;
- exactly one frame;
- several frames;
- a split at any code, data, or delimiter boundary.

The span is only borrowed for the duration of the call. The decoder consumes
it synchronously and retains no pointer into it.

### 5.2 Two-stage zero-copy receive

The receiver first decodes the one- or two-byte length field into local state.
Only after the declared body length `N` is known does it call
`storage.acquire_rx(N)`. The body then decodes directly into the final block.

```text
wire bytes
   -> local length field
   -> validate N
   -> acquire exactly N payload bytes
   -> decode body into RxBlock payload
   -> publish on delimiter
```

There is no encoded staging buffer, second decoding pass, or post-allocation
payload copy.

### 5.3 Ready queue

Each completed `RxBlock` contains its own `next_ready` link. The queue is
therefore intrusive:

- no queue-node allocation;
- O(1) enqueue and dequeue;
- queue capacity naturally bounded by available RX blocks;
- ready packets are unaffected by a later transport gap.

### 5.4 Packet ownership

A freshly constructed RX block begins with one reference:

```text
building owner (refs = 1)
    -> ready queue (same reference)
    -> Packet returned by pop_packet() (same reference)
```

No counter change occurs along the normal path. Copying a `Packet` increments
the block reference count; destroying or resetting a handle decrements it.
The last handle calls `release_rx()` through the typed storage pointer kept in
the block.

The application sees only `std::span<const uint8_t>`. Payload bytes are
immutable after publication; private reference and queue metadata remain
mutable internally.

## 6. TX data and ownership flow

### 6.1 Message construction

`make_message(hint)` acquires a TX block and returns a move-only builder.
The hint is a capacity request, not an initial logical size. `size()` starts at
zero. The no-argument overload requests
`min(max_send_size, 32)` bytes.

`Message` has explicit states:

```text
Empty -> Building -> Encoded
```

While Building, `append_native()`, `append_bytes()`, and `reserve()` may grow
capacity by approximately 1.5x. Growth acquires a replacement first, copies
only the application bytes, and releases the old block only after success.
A failed growth leaves pointer, capacity, size, and contents unchanged.

### 6.2 Send outcomes

`Endpoint::send(Message&)` never consumes the caller's object blindly:

| Result | Ownership and state after return |
|---|---|
| `Sent` | transport accepted the span; endpoint owns the block; message is Empty |
| `Busy` | active TX or busy transport; message is unchanged |
| `Unbound` | no complete delegate pair; message is unchanged |
| `Failed` | sender refused after encoding; message remains Encoded and retryable |
| `Invalid` | empty message or storage belongs to another endpoint; unchanged |

Busy and unbound checks happen before encoding, so a Building message remains
appendable. Once encoding happened, the raw body no longer exists separately;
a failed start therefore retains the exact encoded bytes for retry.

### 6.3 Active transport borrow

On `Sent`, ownership moves from `Message` to `Endpoint::m_activeTx`. The
transport borrows the returned byte span but never owns or frees it.
`poll()` asks the paired busy delegate only while an active block exists.
When it reports false, the endpoint returns the complete `TxBlock` descriptor
to storage.

`busy() == false` means only that the transport has stopped reading the
buffer. It is not proof of delivery. Hardware completion/error reporting
belongs to the transport.

The block is released from `poll()` rather than an ISR callback. Storage is
therefore not required to be interrupt-safe merely to reclaim TX memory.

## 7. Transport binding and delegate lifetime

`bind(sender, busy)` accepts both delegates transactionally:

- either empty delegate rejects the bind;
- a rejected bind leaves the established pair unchanged;
- bind, rebind, and unbind are rejected while a TX block is active;
- `unbind()` is the only removal operation.

The type is the owning `tiny::delegate`, and all intentionally supported
binding modes remain available:

- an owned lambda, including a move-only capture;
- `tiny::bind<&Type::method>(object)` for a long-lived target object;
- `tiny::borrow(callable)` for an explicitly externally owned callable.

“Owning delegate” describes the delegate container, not every target mode.
Objects referenced by `tiny::bind` or `tiny::borrow` must outlive the binding.
The endpoint cannot repair a dangling external target.

## 8. Lifetime and execution-domain contract

The following are preconditions, not optional advice:

1. An endpoint outlives every `Packet` and `Message` created from it. Both may
   later call its embedded storage object.
2. An endpoint is not destroyed while `tx_active()` is true or while the
   transport still borrows its frame. Drain with `poll()` first.
3. A message is sent only through the exact endpoint instance that created it.
   Two endpoints with the same `StorageT` are still different owners;
   `send()` detects the mismatch and returns `Invalid`.
4. Calls that mutate an endpoint are externally serialized. The endpoint,
   decoder, pool, message, and plain packet refcount contain no locks.
5. Packet copying/releasing stays in one execution domain in v1. Cross-task
   sharing would require a separately designed atomic reference policy.
6. `consume()` and `notify_gap()` are presented in real stream order. A gap
   moves decoding into discard-until-delimiter state even if it appeared to
   occur between known frames.
7. Sender and busy-query delegate targets do not throw and do not re-enter
   `bind()`, `unbind()`, `send()`, or `poll()` on the same endpoint. They run
   synchronously inside `noexcept` methods; throwing terminates, while
   re-entry can interrupt the TX ownership hand-off.
8. A sender returns `true` only after borrowing the supplied span, and keeps
   that borrow until the paired busy query returns `false`. Returning `false`
   means no borrow was taken. The busy query is side-effect-free and never
   reports idle while hardware can still read the block.

## 9. Observability

`Endpoint::stats()` returns one `cobs::Stats` value:

```cpp
const cobs::Stats snapshot = endpoint.stats();
```

RX and TX counters remain physically beside the state transitions that update
them, but callers cannot mutate them or retain references into engine state.
The RX snapshot distinguishes delivered/lost frames, allocation failures,
structural malformed frames, oversize declarations, length mismatches, and
resynchronizations. TX distinguishes accepted frames, busy refusals, and
sender-start failures.

Pool occupancy and pool allocation/release statistics are storage-specific.
They remain available through the const `endpoint.storage()` view and are not
folded into protocol statistics.

Event counters are native `uint32_t` increments and wrap modulo 2^32. A
long-running monitor extends periodic modular deltas if it needs a wider
lifetime total; the protocol hot path does not pay for 64-bit or saturating
arithmetic on Cortex-M.

## 10. Architectural invariants

Any future change must preserve these unless the protocol and architecture are
explicitly revised together:

1. UART or another transport handles bytes; COBS handles frames and packets.
2. `Endpoint` has one template parameter: storage.
3. Transport remains one paired sender/busy owning-delegate boundary.
4. `Format` is the only source of protocol limits and length width.
5. Storage is the only source of memory strategy and quotas.
6. RX allocation is exact and the body decodes directly into final storage.
7. RX ownership stays typed and intrusive; no per-packet `void*` deleter.
8. TX ownership stays exclusive and moves only after transport acceptance.
9. `TxBlock` keeps pointer and granted payload capacity together until release.
10. A failed sender start retains one byte-identical encoded frame for retry.
11. A gap is absorbed below the application packet boundary.
12. The public application include does not expose `detail` types.
13. Heap and Pool using the same `Format` emit the same bytes.
14. There is no internal TX queue or delivery-acknowledgement fiction.

## 11. Reading order

- Start here for components, API, and ownership.
- Read `PROTOCOL.md` when implementing a peer or reviewing framing changes.
- Read `STORAGE.md` when selecting or writing a memory strategy.
- Read `UART_PARANOID_AUDIT.md` for the independent STM32 DMA byte transport.
- Read `BUILD.md` for exact verified commands.
- Read `COBS_ENGINE.md` for the detailed rationale and overlap proof.
- Read `COBS_REFACTOR_PLAN.md` for locked migration decisions and history.
