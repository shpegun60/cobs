# COBS refactor plan

Status: canonical plan, implementation in progress

Created: 2026-09-01

Baseline: `main` at `928f223b0017d4a474df51cb9be7179f53644d47`

This document is the durable plan for restructuring the active COBS library.
It records the decisions that must survive file moves and renames, the target
API and ownership model, the order of migration, and the evidence required at
each step. It is deliberately more detailed than the future user-facing
architecture documentation.

`COBS_ENGINE.md` remains the implementation contract for the current engine
until the migration is complete. If this plan and that contract appear to
disagree about current behaviour, `COBS_ENGINE.md` wins. This document defines
where the architecture is going and explicitly identifies the invariants that
must not change on the way there.

## 1. Purpose

The existing algorithms are not the reason for the refactor. The decoder,
exact-size RX allocation, intrusive shared ownership, in-place TX encoding,
and single-active-transfer handshake are sound and heavily characterized.
The problems are architectural:

1. application types, extension points, and implementation types all look
   equally public;
2. the allocator policy also determines wire geometry;
3. the allocator contract is prose rather than a checked C++ interface;
4. related ownership fields are represented by duplicate descriptors;
5. several lower layers include a concrete default policy only to provide a
   default template argument;
6. application-facing types expose methods used only by their coordinator;
7. the root Qt project does not compile the COBS core;
8. current contracts and historical design sketches are mixed together;
9. a small amount of UART packaging debris distracts from an otherwise sound
   driver.

The goal is therefore a clean C++20 architecture with explicit boundaries,
short names, checked extension contracts, and the same runtime semantics.

## 2. Locked decisions

These decisions are constraints, not suggestions. A migration step that
violates one of them is a redesign and must not be merged as part of this
refactor.

### 2.1 The project remains idiomatic C++20

- Keep templates, concepts, RAII, move-only ownership, private fields, typed
  pointers, `std::span`, and value semantics where they already fit.
- Do not replace owners with `void*`, manual thunks, manual vtables, or C-style
  context/function-pointer pairs.
- Do not turn the public surface into opaque C handles.
- Do not make internal state public merely to simplify a policy interface.

### 2.2 Transport delegates remain owning `tiny::delegate`

- The sender and busy query remain a pair of owning `tiny::delegate` objects.
- Owning lambdas, bound members, and borrowed callables remain supported.
- Do not replace either delegate with `delegate_ref`, a raw function pointer,
  `std::function`, a virtual transport, or a transport template parameter.
- They may be grouped in a private nested `Transport` class so their invariant
  is localized, but their semantics and lifetime model must remain unchanged.

### 2.3 Explicit state machines remain explicit

- Keep every decoder state and counter needed by streaming COBS decoding.
- Keep the receiver's explicit `Header`/`Body` stage, declared length, and
  header-attachment state.
- Keep the message's explicit state.
- Do not infer state from `nullptr`, size, capacity, or wire size in order to
  save a few bytes. The resulting hidden state machine would be harder to
  audit and easier to corrupt.

### 2.4 Protocol bytes must remain identical

- Wire format remains `COBS([length][body]) 00`.
- The length prefix remains fixed-width, one or two bytes, selected from the
  larger directional limit.
- The length prefix remains little-endian.
- Existing scalar writes remain native-representation writes. Renaming them
  to `append_native` makes that fact explicit; it does not change byte order,
  enum width, or floating-point representation.
- CRC, authentication, version negotiation, and new framing fields are out of
  scope.

### 2.5 Ownership and allocation behaviour must remain unchanged

- RX still decodes its length header first, validates it, performs one exact
  allocation, then decodes the body directly into the final packet.
- An RX allocation remains one contiguous `[block metadata][payload]` region.
- Completed RX packets remain immutable, intrusive, shared handles.
- TX messages remain move-only exclusive owners while being built.
- TX encoding remains canonical, in-place, and zero-copy to the transport.
- There remains at most one active TX transfer and no internal TX queue.
- RX and TX quotas remain independent in deterministic storage.
- Both heap-backed and deterministic static storage remain supported.
- `Endpoint` owns its storage object by value unless a separately reviewed
  future design establishes a different lifetime contract.

### 2.6 UART remains a byte transport

- COBS consumes bytes and gap notifications; UART does not learn about COBS
  packets.
- COBS sends a byte span and asks whether that transfer is still busy; it does
  not learn UART register or DMA details.
- No combined production `UartCobs` class is planned. Composition is shown in
  an example and performed through the delegate pair.
- UART ISR, DMA, watchdog, recovery, and port-selection logic are frozen during
  the COBS restructuring.

## 3. Verified starting point

The refactor starts from clean `main` at the SHA above. The characterization
baseline recorded before production changes is:

- COBS host suite: 22,987 checks, zero failures;
- COBS release-mode allocator/pool checks included;
- UART host suite: 131 checks, zero failures;
- STM32 F1, G4, and H7RS compile matrix passed;
- disabled-probe and port-equivalence checks passed.

The root `COBS.pro` currently builds only the Qt GUI scaffold. Passing that
build alone is not evidence that either COBS or UART compiles.

### 3.1 Host layout snapshot

The following values were measured on the pre-refactor x86-64 MinGW build and
with the repository's `arm-none-eabi-g++` Cortex-M4 configuration. They are
regression clues for those ABIs, not universal ABI promises:

| Current type | x86-64 MinGW | ARM EABI, Cortex-M |
|---|---:|---:|
| pointer / `std::size_t` | 8 / 8 bytes | 4 / 4 bytes |
| `cobs::TxBlock` | 16 bytes | 8 bytes |
| `cobs::RxBlock<cobs::Heap<cobs::Format<...>>>` | 24 bytes | 16 bytes |
| `PacketRef<...>` | 8 bytes | 4 bytes |
| `CobsMsg<...>` | 48 bytes | 24 bytes |
| `cobs::codec::Decoder` | 48 bytes | 24 bytes |
| `CobsRx<...>` | 136 bytes | 80 bytes |
| `Cobs<cobs::Heap<cobs::Format<...>>>` | 304 bytes | 168 bytes |
| `cobs::Heap<cobs::Format<...>>` | 1 byte | 1 byte |
| `tiny::delegate` sender | 64 bytes | 32 bytes |
| `tiny::delegate` busy query | 64 bytes | 32 bytes |
| RX statistics | 28 bytes | 28 bytes |
| TX statistics | 12 bytes | 12 bytes |
| pool statistics | 16 bytes | 16 bytes |
| `cobs::Pool<cobs::Format<1024, 1024>, 8, 2>` | 10,496 bytes | 10,424 bytes |
| `Cobs<cobs::Pool<cobs::Format<1024, 1024>, 8, 2>>` | 10,800 bytes | 10,592 bytes |

The owning delegates are intentionally retained. Their size is not a
reason to weaken their owning-callable semantics. Before layout work is marked
complete, exact assertions must pass under both host and the recorded ARM
compiler because padding and pointer width differ.

## 4. Current runtime architecture

### 4.1 Receive path

```text
byte chunks
    |
    v
CobsRx<StorageT>
    |
    +-- cobs::codec::Decoder decodes the length header into a local 1-2 byte buffer
    +-- the declared body length is parsed and validated
    +-- StorageT::acquire_rx(length) obtains the exact final RxBlock
    +-- the same decoder is attached to that packet's writable payload
    +-- a complete packet moves to the intrusive ready queue
    +-- pop_packet() transfers the queue's existing reference to PacketRef
```

The normal acquisition-to-pop path does not increment or decrement the
reference count. Ownership moves logically from `building`, to the ready
queue, to the returned handle. Only copying or destroying a public packet
handle changes the count.

### 4.2 Transmit path

```text
StorageT::acquire_tx(hint) -> cobs::TxBlock
    |
    v
CobsMsg<StorageT> builds a body and grows geometrically when required
    |
    v
length prefix is written before the body
    |
    v
canonical COBS frame is encoded in the same block
    |
    v
Cobs::push() gives the encoded span to the sender delegate
    |
    +-- refused: message retains its TxBlock
    +-- accepted: Cobs owns the active TxBlock until busy() becomes false
```

The transport borrows the encoded bytes. The engine remains responsible for
returning the original TxBlock descriptor to storage unchanged.

## 5. Target vocabulary

The names below are the desired final vocabulary. Phase 0 deliberately adds a
transitional contract over current names; it is not permission to expose two
permanent APIs.

| Current name | Target name | Layer | Meaning |
|---|---|---|---|
| `Cobs` | `cobs::Endpoint` | application | assembled RX/TX endpoint |
| `CobsMsg` | `cobs::Message` | application | move-only TX builder/owner |
| `PacketRef` | `cobs::Packet` | application | immutable shared RX handle |
| `SendResult` | `cobs::SendResult` | application | result of starting TX |
| separate RX/TX accessors | `cobs::Stats` | application | value snapshot of counters |
| `CobsFrameFormat` helpers | `cobs::Format<Rx, Tx>` | extension | protocol geometry only |
| allocator policy idea | `cobs::Storage` | extension | checked storage contract |
| `CobsHeapAllocator` | `cobs::Heap<Format>` | extension | dynamic memory strategy |
| `CobsFixedAllocator` | `cobs::Pool<Format, RxN, TxN>` | extension | deterministic memory strategy |
| `TxAllocation` and `CobsMsg::TxBlock` | `cobs::TxBlock` | extension | one TX ownership descriptor |
| `RxPacket` | `cobs::RxBlock<Storage>` | extension | typed storage-extension block |
| `CobsDecoder` | `cobs::codec::Decoder` | low-level | non-template streaming decoder |
| `cobs_encode_in_place` | `cobs::codec::encode_in_place` | low-level | canonical in-place encoder |
| `CobsRx` | `cobs::detail::Receiver` | internal | RX assembly and ready queue |
| `StaticBlockPool` | `cobs::detail::BlockPool` | internal | fixed-block implementation |

Target method vocabulary:

| Current | Target | Notes |
|---|---|---|
| `set_transport` | `bind` | binds both owning delegates atomically |
| no direct counterpart | `unbind` | explicit removal of both delegates |
| `make_msg` | `make_message` | returns a move-only builder |
| `push` | `send` | attempts to start one transfer |
| `proceed` | `poll` | releases a completed active TX block |
| `gap` | `notify_gap` | reports a transport-level RX gap |
| `allocator` | `storage` | exposes the selected memory strategy |
| `write_bytes` | `append_bytes` | appends an explicit byte range |
| `write` for scalars | `append_native` | states native representation honestly |
| `pop_packet` | `pop_packet` | already precise; keep it |
| `has_packet` | `has_packet` | already precise; keep it |

Final send-result enumerators are intended to be `Sent`, `Busy`, `Unbound`,
`Failed`, and `Invalid`. The rename from `NotBound`/`Error` is vocabulary only.

## 6. Target dependency structure

```text
Application
    |
    v
cobs::Endpoint<Storage>
    |
    +-- cobs::Message<Storage>          TX builder and exclusive owner
    +-- cobs::Packet<Storage>           immutable shared RX handle
    +-- cobs::detail::Receiver<Storage> RX assembly and ready queue
    +-- cobs::codec                     non-template codec implementation
    +-- Storage                         Heap / Pool / custom implementation
    |
    v
private Transport { Sender, BusyQuery }
    |
    v
UART / TCP / test transport
```

Dependency rules:

1. Application code normally includes only `cobs/Cobs.h`.
2. A custom storage author includes `cobs/Storage.h`.
3. A low-level codec user includes `cobs/Codec.h`.
4. Public headers may include detail definitions required by templates, but
   detail names are not application contracts.
5. `Receiver` must not include `Heap` merely to obtain a default argument.
6. Codec code depends on neither storage nor transport.
7. Storage depends on protocol geometry, never the reverse.
8. UART and COBS remain independently testable.

## 7. Target application API

Illustrative final use:

```cpp
using Wire = cobs::Format<
    1024, // maximum received body
    64>;  // maximum transmitted body

using Memory = cobs::Pool<
    Wire,
    8,  // RX blocks
    2>; // TX blocks

cobs::Endpoint<Memory> link;

link.bind(sender, busy); // both are owning tiny::delegate objects
link.consume(bytes);
link.notify_gap();

auto message = link.make_message();
message.append_bytes(payload);
message.append_native(command);

switch (link.send(message)) {
case cobs::SendResult::Sent:
case cobs::SendResult::Busy:
case cobs::SendResult::Unbound:
case cobs::SendResult::Failed:
case cobs::SendResult::Invalid:
    break;
}

link.poll();

while (auto packet = link.pop_packet()) {
    process(packet.data());
}
```

The default endpoint remains heap-backed. Deterministic users select `Pool`
explicitly. A change of storage implementation must not accidentally select a
different length-header width.

### 7.1 Public versus coordinator-only message operations

Application-facing `Message` should expose building and inspection operations:

- truth/value check;
- payload `size()` and `capacity()`;
- `append_bytes`, `append_native`, and array/span forms;
- `clear` if its exact current semantics are retained.

Encoding state transitions, storage identity checks, and block surrender are
coordinator mechanics. `encode()`, `encoded()`, `belongs_to()`, and
`surrender_block()` should become private or detail-facing once `Endpoint` is
the only legitimate caller. This change must be made with focused state and
lifetime tests, not as part of a broad rename.

## 8. Format and storage architecture

### 8.1 Separate protocol geometry from memory strategy

Before Phase 2, policies exposed `rx_max_size` and `tx_max_size`, and
`CobsFormatFor<Allocator>` derived length geometry from them. Consequently,
changing a memory strategy could also change the protocol type. That reverse
dependency has now been removed: each storage names one `cobs::Format`, and
the engine reads protocol geometry only from that type.

The target separates these choices:

```text
Format<1024, 64>        protocol geometry
Heap<Format>            memory strategy A
Pool<Format, 8, 2>      memory strategy B
```

`Format` owns only compile-time framing facts:

- maximum RX body size;
- maximum TX body size;
- larger directional maximum;
- `LengthType`;
- length-prefix width;
- validated encoded-size arithmetic.

`Heap` and `Pool` name their `Format` type and implement allocation. They do
not independently redeclare protocol limits.

### 8.2 Why this is not `std::allocator`

The domain has asymmetric object lifetimes and two independent resource
classes:

- RX returns a constructed typed block followed by exact payload storage;
- TX returns a raw byte block plus the usable payload capacity it represents;
- static storage has independent RX/TX quotas and observable exhaustion;
- the receiver and shared packet handle participate in intrusive ownership.

Forcing this through `std::allocator` or PMR would hide rather than standardize
the actual contract. The correct standardization mechanism is a narrow C++20
concept plus behavioral conformance tests.

### 8.3 Checked transitional storage concept

Phase 0 first checked the existing operations. Phase 2 moved directional
limits into the required `Format` type without yet renaming the four memory
operations:

```cpp
template<class T>
concept Storage = requires(
    T& storage,
    std::size_t size,
    typename T::Packet* packet,
    std::byte* memory)
{
    typename T::Packet;
    typename T::Format;

    { T::Format::max_receive_size } -> std::convertible_to<std::size_t>;
    { T::Format::max_send_size } -> std::convertible_to<std::size_t>;

    { storage.allocate_rx(size) }
        noexcept -> std::same_as<typename T::Packet*>;

    { storage.deallocate_rx(packet) }
        noexcept -> std::same_as<void>;

    { storage.allocate_tx(size) }
        noexcept -> std::same_as<TxAllocation>;

    { storage.deallocate_tx(memory, size) }
        noexcept -> std::same_as<void>;
};
```

This transitional form was removed in Phase 3. It is retained here only as the
recorded migration seam; no forwarding concept, trait, or alias remains in the
active API.

### 8.4 Final storage concept

The active contract is now:

```cpp
template<class T>
concept Storage = requires(
    T& storage,
    std::size_t size,
    typename T::RxBlock* rx,
    cobs::TxBlock tx)
{
    typename T::Format;
    typename T::RxBlock;
    requires std::same_as<typename T::RxBlock, cobs::RxBlock<T>>;

    { T::Format::max_receive_size }
        -> std::convertible_to<std::size_t>;
    { T::Format::max_send_size }
        -> std::convertible_to<std::size_t>;

    { storage.acquire_rx(size) }
        noexcept -> std::same_as<typename T::RxBlock*>;

    { storage.release_rx(rx) }
        noexcept -> std::same_as<void>;

    { storage.acquire_tx(size) }
        noexcept -> std::same_as<cobs::TxBlock>;

    { storage.release_tx(tx) }
        noexcept -> std::same_as<void>;
};
```

The concept checks syntax and exception guarantees. Shared behavioral tests
must still prove capacity bounds, non-overlap, lifetime, exact deallocation,
independent quotas, and exhaustion behaviour.

### 8.5 RX block contract

`RxBlock<Storage>` remains a typed C++ object. Its metadata remains private:

- typed `Storage* owner`;
- reference count;
- decoded payload size;
- intrusive next-ready link.

Storage may construct and destroy the block, but it must not arbitrarily
mutate reference count, decoded size, or queue linkage. Friendship should be
the narrowest mechanism that permits construction and release.

The current `uint32_t` reference count is preserved. Making packet handles
thread-safe with atomics would be a separate semantic and layout decision.

### 8.6 TX block contract

The former `TxAllocation` and nested `CobsMsg::TxBlock` were replaced by one
ownership record:

```cpp
struct TxBlock {
    std::byte* memory = nullptr;
    std::size_t capacity = 0;
};
```

That descriptor is transferred as a unit from
storage, to `Message`, to `Endpoint`, then back to storage. A pointer without
its reported capacity should not be expressible at that boundary.

`capacity` is the payload capacity promised by storage, not necessarily raw
allocation byte count. Encoder geometry continues to derive the full block
size safely from it.

## 9. Runtime fields and their owners

The code currently has many structures because the algorithm has several
real lifetimes. The refactor groups fields by owner; it does not flatten
those lifetimes into a single bag of state.

Exact field migration inventory:

| Current owner | Current fields | Target decision |
|---|---|---|
| `cobs::codec::Decoder` | `m_state`, `m_output`, `m_written`, `m_decodedBefore`, `m_blockRemaining`, `m_pendingZero`, `m_hasOutput` | retained unchanged by the completed codec namespace move |
| `CobsRx` | `m_decoder`, `m_storage`, `m_lengthBytes`, `m_declared`, `m_stage`, `m_headerAttached`, `m_building`, `m_readyHead`, `m_readyTail`, `m_stats` | storage vocabulary completed; queue writes still move to private helpers |
| `cobs::RxBlock<Storage>` | `refs`, `size`, `next_ready`, `owner` | typed/private metadata retained unchanged |
| `PacketRef` | `m_p` | retain the one-pointer handle; target type is `Packet<Storage>` |
| `CobsMsg` | `m_storage`, `m_block`, `m_size`, `m_wire`, `m_state` | pointer+capacity now travel in one `cobs::TxBlock`; explicit state remains |
| `Cobs` | `m_storage`, `m_rx`, `m_sender`, `m_txBusy`, `m_activeTx`, `m_txStats` | storage and receiver remain; delegates may be grouped in private `Transport`; active descriptor and counters remain |
| `cobs::Pool` | `m_rx`, `m_tx` | two independent pools retained |
| `StaticBlockPool` | `m_blocks`, `m_free`, `m_stats` | retain under `detail::BlockPool`; no public ownership role |

Current counter fields are also preserved: RX has `frames_delivered`,
`frames_lost`, `allocation_failure`, `malformed`, `oversize`,
`length_mismatch`, and `resyncs`; TX has `frames_sent`, `send_refused_busy`,
and `send_failed`; each fixed pool tracks `in_use`, `high_water`, `exhausted`,
and `rejected` independently.

### 9.1 `cobs::codec::Decoder`

Keep:

- decoder state;
- current output span and write position;
- COBS segment/code counters;
- pending implicit-zero information;
- frame byte/output counters;
- malformed/completion flags required by the state machine.

Reason: each represents streaming progress across arbitrary input chunk
boundaries. Removing one generally recreates it as an implicit condition.

### 9.2 `cobs::detail::Receiver<Storage>`

Keep:

- decoder;
- storage reference;
- one- or two-byte length buffer;
- parsed declared length;
- explicit `Header`/`Body` stage;
- header-attached flag;
- packet currently being built;
- ready-queue head and tail;
- RX counters.

Queue manipulation should be centralized in private `enqueue_ready`,
`dequeue_ready`, and `clear_ready` helpers. A new public queue abstraction is
not necessary.

### 9.3 `cobs::Message<Storage>`

Keep:

- typed `Storage*` owner;
- one `TxBlock` descriptor;
- payload size;
- encoded wire size;
- explicit message state.

The descriptor replaces separate pointer/capacity fields only when it can do
so without changing layout or behaviour unexpectedly. The explicit state is
not replaced by descriptor nullness.

### 9.4 `cobs::Packet<Storage>`

Keep the public handle small: one pointer to its typed RX block. Copying adds a
reference, moving transfers it, and destruction releases it through the typed
storage owner. Payload remains exposed as immutable data.

### 9.5 `cobs::Endpoint<Storage>`

Keep:

- storage by value;
- receiver bound to that storage;
- private transport pair containing sender and busy delegates;
- active `TxBlock`;
- TX counters.

`Endpoint` stays non-copyable and non-movable while outstanding objects hold a
pointer to its embedded storage. Its destructor precondition regarding an
active borrowed TX buffer remains documented and tested where possible.

### 9.6 Statistics

The final application snapshot is:

```cpp
struct Stats {
    RxStats rx;
    TxStats tx;
};

[[nodiscard]] Stats stats() const noexcept;
```

Physical counters may remain next to the state transitions that update them.
Pool-specific occupancy/exhaustion statistics remain storage-specific and are
not folded into protocol statistics.

## 10. RX ownership transitions

```text
Idle/Header
    |
    | decoded length becomes known and valid
    v
storage.acquire_rx(N)
    |
    | one initial reference belongs to Receiver::building
    v
Body decoding directly into final payload
    |
    +-- malformed/gap/allocation path -> destroy/release exactly once
    |
    v
FrameComplete
    |
    | same reference, no refcount arithmetic
    v
ready queue
    |
    | pop_packet adopts the same reference
    v
Packet handle
    |
    +-- copy -> increment
    +-- move -> no increment
    +-- last destruction -> release through typed Storage owner
```

Tests must cover complete frames, zero-length bodies, allocation failure,
oversize declarations, malformed frames, gaps in each stage, ready-queue
cleanup, copied handles, moved handles, and endpoint destruction with queued
but unpopped packets.

## 11. TX ownership transitions

```text
Empty
    |
    | make_message(hint), storage returns TxBlock
    v
Building
    |
    +-- append fits -> same block
    +-- append grows -> acquire new, copy body, release old
    +-- clear/reset -> preserve the documented current semantics
    |
    | Endpoint encodes before transport handoff
    v
Encoded
    |
    +-- Busy/Unbound/Failed/Invalid -> Message keeps its block
    |
    +-- Sent -> descriptor moves to Endpoint::activeTx; Message becomes Empty
                       |
                       | poll observes busy delegate false
                       v
                  storage.release_tx(active block)
```

Tests must prove that a failed send leaves a retryable message, an accepted
send empties it, no second transfer starts while active, the exact original
capacity is returned, growth never leaks the previous block, moves release
exactly once, and destruction does not release bytes still borrowed by a
transport.

## 12. Intended file layout

```text
cobs/
|-- Cobs.h                 application umbrella and Endpoint
|-- Format.h               protocol geometry only
|-- Storage.h              Storage, RxBlock, TxBlock, Heap, Pool
|-- Codec.h                low-level codec API
|-- Decoder.cpp
|-- Encoder.cpp
`-- detail/
    |-- Message.h
    |-- Packet.h
    |-- Receiver.h
    `-- BlockPool.h
```

This is a target organization, not a requirement to perform one large file
move. Each slice is nevertheless a real move: repository consumers change
together, and forwarding headers or compatibility aliases are not retained.

## 13. Build and integration boundary

The repository needs a build target that proves the real library is usable.
The final integration work should:

- add `cobs/cobs.pri` or an equivalent reusable qmake fragment;
- compile non-template codec sources exactly once;
- expose the required include paths for COBS and `tiny_delegate`;
- add a small consumer that constructs an `Endpoint`, binds delegates, sends,
  receives, and exercises both default and deterministic storage at compile
  time;
- keep host test scripts available independently of Qt;
- document the exact MinGW, WSL sanitizer, and STM32 matrix commands.

The empty GUI scaffold may remain an example shell, but its success must no
longer be confused with core-library validation.

## 14. UART follow-up boundary

UART cleanup is a separate, later phase after the COBS API is stable:

- confirm and remove unused `uart/basic_types.h`;
- replace `_DELETE_COPY_MOVE` in `IRQGuard` with explicit deleted C++ special
  members, then remove `uart/macro.h` if no uses remain;
- place registry, IRQ, and register helpers under `uart::detail` where this is
  a namespace-only change;
- do not alter interrupt ordering, DMA ownership, gap detection, watchdog
  timing, recovery, HAL callback registration, or port probes;
- rerun the complete UART host and F1/G4/H7RS matrix after every UART slice.

The COBS refactor must not opportunistically include these changes.

## 15. Documentation end state

When the target API is stable, split durable documentation by audience:

- `ARCHITECTURE.md`: short component boundaries and ownership overview;
- `PROTOCOL.md`: exact framing, length geometry, size arithmetic, errors;
- `STORAGE.md`: concept, runtime obligations, custom implementation guide;
- `BUILD.md`: supported build/test commands and toolchain assumptions;
- this plan: migration history and decision record.

Only after content parity is checked should obsolete design sketches move to
`doc/old`. `UART_COBS_ARCHITECTURE.md` must not silently remain an apparently
current source of truth when its API examples are historical.

## 16. Migration phases

Each phase should be reviewable on its own. Renames, ownership changes, and
protocol changes must never be bundled together.

### Phase 0 - characterization and checked current contract

- [x] Record the verified pre-refactor baseline.
- [x] Add self-contained compile smoke tests for each public header.
- [x] Record host and ARM-relevant object-layout probes.
- [x] Add the transitional `cobs::Storage` concept over current method names.
- [x] Assert that both built-in policies satisfy it.
- [x] Add negative compile-time checks for missing/wrong/non-`noexcept`
      operations.
- [x] Keep production runtime behaviour byte-for-byte unchanged.

### Phase 1 - namespace and vocabulary skeleton

- [x] Physically move the non-template codec to `cobs::codec::Decoder`,
      `cobs::codec::encode_in_place`, and the concise geometry names in
      `Codec.h`; rename the implementation files to `Decoder.cpp` and
      `Encoder.cpp`.
- [x] Take a clean API break: do not add compatibility aliases or traits for
      old names. All repository consumers move with each real rename slice.
- [ ] Move the remaining application and detail types into
      `namespace cobs` under their final names.
- [ ] Complete the vocabulary without changing state transitions or framing.
- [x] Keep delegates exactly as currently implemented.

### Phase 2 - protocol `Format`

- [x] Introduce `Format<MaxReceive, MaxSend>`.
- [x] Make both built-in storage strategies accept and name one format type.
- [x] Remove the reverse dependency from format to allocator constants.
- [x] Test complementary peers and length-width boundaries at 0, 254, 255,
      256, and 65535 where supported.
- [x] Prove heap and pool with the same `Format` emit identical frames.

### Phase 3 - final storage vocabulary

- [x] Introduce the single `TxBlock` descriptor.
- [x] Introduce typed `RxBlock<Storage>`.
- [x] Migrate `allocate/deallocate` to `acquire/release` in one controlled
      slice.
- [x] Update `Storage` from transitional to final form.
- [x] Preserve independent RX/TX quotas and exact release values.
- [x] Add a minimal custom-storage conformance example.

### Phase 4 - close the internal surface

- [ ] Move receiver and block-pool implementation under `cobs::detail`.
- [ ] Make message encoding, ownership checks, and surrender coordinator-only.
- [ ] Centralize ready-queue mutations in receiver helpers.
- [ ] Keep typed ownership and private metadata.
- [ ] Verify packet/message lifetime and endpoint-destruction edges.

### Phase 5 - clean application API

- [ ] Apply method renames from the vocabulary table.
- [ ] Add explicit `unbind` with a documented active-transfer precondition.
- [ ] Group the two delegates in private `Transport` if layout/behaviour proof
      shows no regression.
- [ ] Return the combined `Stats` snapshot.
- [ ] Update examples to include only `Cobs.h`.

### Phase 6 - tests around public boundaries

- [ ] Separate codec, storage, message, packet-lifetime, endpoint, public API,
      and compile-fail tests.
- [ ] Permit white-box tests to include `detail` explicitly.
- [ ] Require application tests to include only `Cobs.h`.
- [ ] Add wire-equivalence fixtures across storage implementations.
- [ ] Add transport-delegate lifetime tests for owning lambdas/binds/borrows.

### Phase 7 - build and documentation

- [ ] Add a real COBS consumer/build fragment to qmake.
- [ ] Produce `ARCHITECTURE.md`, `PROTOCOL.md`, and `STORAGE.md` from the stable
      API.
- [ ] Update `BUILD.md` with exact verified commands.
- [ ] Check parity, then archive superseded sketches.
- [x] Retain no forwarding aliases or headers during the migration.

### Phase 8 - isolated UART hygiene

- [ ] Remove only proven-unused packaging files.
- [ ] Replace macro-generated deleted operations with explicit C++.
- [ ] Hide helpers under `uart::detail` without logic changes.
- [ ] Rerun every host, port, probe, and equivalence check.

## 17. Verification matrix

Every production slice must run the checks relevant to its touched boundary.
Before a phase is declared complete, the full matrix is required:

| Area | Required evidence |
|---|---|
| COBS host | all existing 22,987+ checks pass under MinGW |
| Release guarantees | allocator and block-pool tests pass with `-DNDEBUG` |
| Sanitizers | COBS suite passes under WSL with ASan and UBSan |
| Header hygiene | every supported public header compiles in an otherwise empty TU |
| Storage syntax | built-ins satisfy the concept; invalid policies fail clearly |
| Storage behaviour | shared contract suite passes for Heap, Pool, and example custom storage |
| Wire compatibility | reference encoder and heap/pool frames are byte-identical |
| Ownership | message moves/growth/send retry and packet copy/move/release pass |
| Layout | host and ARM probes reviewed; unexpected growth explained before merge |
| UART host | all 131+ checks pass |
| STM32 ports | F1, G4 variants, and H7RS compile matrix passes |
| Probe contract | disabled-probe disassembly and equivalence checks pass |
| Qt integration | real consumer compiles the core, not only the GUI scaffold |
| Repository | `git diff --check` clean; documentation matches the exposed API |

A partial build must be reported as partial. `-fstack-usage` output, if added,
is individual-frame evidence and must not be described as cumulative call-chain
proof.

## 18. Acceptance criteria

The refactor is complete only when all of the following are true:

1. An application can understand the primary API from `Endpoint`, `Message`,
   `Packet`, `SendResult`, and `Stats` without seeing receiver/block internals.
2. A storage author has one documented header, one checked concept, and one
   behavioral conformance suite.
3. Heap and deterministic pool can share one `Format` and therefore one wire
   protocol.
4. `TxBlock` is the only TX ownership descriptor.
5. RX ownership remains typed and intrusive; no `void*` owner is introduced.
6. Explicit decoder, receiver, and message states remain visible in code.
7. The sender/busy pair remains owning `tiny::delegate`.
8. COBS and UART remain separate and independently testable.
9. The root/consumer build compiles real core code.
10. The full verification matrix passes on the final tree.
11. Current and historical documentation are visibly separated.
12. No temporary alias, forwarding header, or duplicated API remains without a
    documented removal decision.

## 19. Explicit non-goals

The following are not part of this refactor:

- replacing delegates;
- converting the project to C;
- virtual transport interfaces;
- internal TX queueing;
- multiple simultaneous TX transfers;
- CRC or protocol-version fields;
- changing endian or scalar representation;
- atomic/thread-safe packet references;
- rewriting proven decoder/encoder algorithms for style;
- redesigning UART ISR/DMA/recovery behaviour;
- combining UART and COBS into one production type.

## 20. Progress log

### 2026-09-01

- Re-audited the active COBS and UART sources and recorded the baseline.
- Locked the C++20, owning-delegate, explicit-state, protocol, and ownership
  constraints above.
- Created this canonical plan.
- Added the transitional `cobs::Storage` concept over the existing two-limit,
  four-operation policy interface.
- Applied the contract guard to `Cobs`, `CobsRx`, and `CobsMsg` without
  constraining or replacing `tiny::delegate`.
- Added positive compile-time checks for the built-in heap and fixed policies,
  plus negative checks for missing TX operations, a wrong TX return type, and
  a throwing RX allocation operation.
- Verified 22,994 COBS checks with MinGW, the same suite with ASan+UBSan under
  WSL, 131 UART host checks, and the complete F1/G4/H7RS port/probe matrix.
- The two former COBS test `-Wshadow` warnings were removed during the storage
  vocabulary slice. The STM32 H7RS vendor `register` warning remains external.
- Added a persistent smoke pass that compiles every current public COBS header
  independently (7 after storage extension types were consolidated).
- Added exact x86-64 and ARM EABI layout assertions plus an inspectable
  Cortex-M object probe. Phase 0 characterization is now complete.
- Physically replaced the old decoder/encoder files and symbols with
  `Codec.h`, `Decoder.cpp`, `Encoder.cpp`, and real `cobs::codec` definitions.
  No aliases or compatibility traits were retained; object layouts and all
  codec/engine checks remained identical.
- Physically replaced `CobsFrameFormat`/`CobsFormatFor` with `cobs::Format` in
  `Format.h`. Storage policies now accept and name a format type; endpoint,
  receiver, and message read limits only from `Storage::Format`. Duplicate
  storage limit fields were removed, and heap/pool wire identity is tested.
- Replaced the allocator-shaped extension API with the final storage boundary:
  `cobs::Heap`, `cobs::Pool`, typed `cobs::RxBlock`, one `cobs::TxBlock`, and
  `acquire/release` operations. Consolidated those extension types in
  `Storage.h`, removed four obsolete headers without forwarding aliases, and
  renamed the shared conformance suite to `test_storage`.
