<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# COBS storage contract

This document is the canonical extension guide for memory strategies used by
`cobs::Endpoint`. It defines both the C++20 syntax checked by the
`cobs::Storage` concept and the runtime obligations that a concept cannot
express.

Applications selecting `Heap` or `Pool` normally include only `Cobs.h`.
Authors of a custom strategy include `Storage.h`.

## 1. Boundary and non-goals

Storage answers one question: where RX and TX memory comes from and how it is
returned. It does not own:

- COBS encoding or decoding;
- the engine length field or protocol limits;
- RX state, ready-queue behavior, or packet reference transitions;
- message growth policy;
- transport delegates or DMA completion;
- application retry/drop/queue policy.

There are no virtual functions or runtime storage dispatch. `StorageT` is the
single template argument of `cobs::Endpoint<StorageT>`.

One storage type serves RX and TX because it may share placement or runtime
configuration. The direction-specific resources and quotas remain independent.

## 2. Exact checked concept

The production concept is:

```cpp
template<class T>
concept Storage = requires(
    T& storage,
    const std::size_t size,
    typename T::RxBlock* rx,
    cobs::TxBlock tx)
{
    typename T::Format;
    typename T::RxBlock;
    requires std::same_as<typename T::RxBlock, cobs::RxBlock<T>>;

    { T::Format::max_receive_size } -> std::convertible_to<std::size_t>;
    { T::Format::max_send_size } -> std::convertible_to<std::size_t>;

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

A conforming type therefore names one `Format`, uses exactly the typed
`cobs::RxBlock<T>`, and exposes four `noexcept` operations with exact return
types.

The concept checks syntax only. It cannot prove allocation size, distinctness,
quota independence, matching release, or safe exhaustion. Those are part of
the behavioral contract and the shared conformance test.

The endpoint constructors are unconditionally `noexcept`. A custom storage
constructor must also be `noexcept` in practice; throwing from it terminates
the process.

## 3. Format owns protocol geometry

Every strategy names exactly one:

```cpp
using Format = cobs::Format<MaxReceiveBody, MaxSendBody>;
```

`Format` owns:

- `max_receive_size`;
- `max_send_size`;
- `length_size` and `LengthType`;
- little-endian length load/store;
- header-inclusive TX size and offset calculations.

Storage must not duplicate those constants or recompute header width. Heap,
pool, external SRAM, and test storage that name the same `Format` speak the
same protocol.

Useful geometry:

```cpp
Format::decoded_size_for_payload(payload_size);
Format::tx_storage_size_for_capacity(capacity);
Format::raw_offset_for_capacity(capacity);
```

The two public limits are body capacities. Physical allocations include
headers and, for TX, worst-case COBS expansion plus the delimiter.

## 4. Ownership descriptors

### 4.1 TX: `cobs::TxBlock`

```cpp
struct TxBlock final {
    std::byte*  memory   = nullptr;
    std::size_t capacity = 0;
};
```

`capacity` is granted application-payload capacity. It is not:

- the raw allocation byte count;
- the current message size;
- the decoded-frame size;
- the encoded wire size;
- necessarily the original request.

The descriptor moves as one unit:

```text
Storage -> Message -> Endpoint active TX -> Storage
```

The exact descriptor returned for a successful acquisition is passed back to
`release_tx()`. A segregated allocator may use `capacity` to recover its size
class without searching. A strategy such as `Heap` may ignore it.

### 4.2 RX: `cobs::RxBlock<StorageT>`

An RX allocation is one contiguous region:

```text
| constructed cobs::RxBlock<StorageT> | requested payload bytes |
^                                      ^
returned pointer                       payload location
```

The payload begins exactly `sizeof(RxBlock)` bytes after the object.
Header and payload cannot be separate allocations.

The private fields are owned by the receiver/packet vertical:

- `refs`: intrusive reference count, initialized by block construction;
- `size`: published payload size;
- `next_ready`: intrusive queue link;
- `owner`: typed pointer to the endpoint's storage instance.

Storage constructs and destroys the block but never writes these fields.
In particular, `detail::Receiver` sets `owner` after acquisition. Requiring
storage to do it would be an invisible fifth operation outside the concept.

## 5. RX operation contract

### 5.1 `acquire_rx(requested_size)`

For `requested_size <= Format::max_receive_size`, the operation either:

- returns null to report exhaustion/failure; or
- returns a pointer to a live, properly aligned, constructed
  `cobs::RxBlock<ThisStorage>` followed by at least `requested_size` writable
  payload bytes.

Each simultaneously live successful acquisition is distinct and non-
overlapping. The region remains exclusively available to the caller until
release.

The endpoint takes storage at its word. It does not receive an RX capacity and
does not probe the allocation. If the strategy cannot provide the whole
requested region, it returns null.

A request beyond `max_receive_size` must fail rather than silently clamp.
The receiver normally rejects such a frame before calling storage, but the
strategy boundary remains defensive.

### 5.2 `release_rx(block)`

For a valid live pointer previously returned by that same instance:

1. destroy the `RxBlock` object;
2. return the complete contiguous region to the correct resource.

Null is a no-op.

A validating strategy must reject a foreign or already-released pointer
before invoking the destructor. If an invalid release is detected, leaking
that block is safer than corrupting a free list or destroying an unrelated
object.

## 6. TX operation contract

### 6.1 `acquire_tx(requested_capacity)`

For an accepted request, return a non-empty `TxBlock` satisfying:

```text
requested_capacity <= block.capacity <= Format::max_send_size
block.memory points to at least
    Format::tx_storage_size_for_capacity(block.capacity)
bytes of writable storage
```

The physical-size formula is:

```text
cobs::codec::max_wire_size(Format::length_size + block.capacity)
```

It is header-inclusive. Sizing only for
`max_wire_size(block.capacity)` is one or two decoded bytes too small.

Request zero is valid. It still needs a real allocation large enough for the
length field, COBS overhead, and delimiter so the canonical empty engine frame
can be sent.

A request above `Format::max_send_size` returns the empty descriptor
`{nullptr, 0}`. Storage does not clamp it.

The grant may depend on strategy:

```text
Heap, exact             request 100 -> capacity 100
Pool, one slab          request 100 -> capacity max_send_size
segregated size classes request 100 -> capacity 128
```

The message owns the growth rule. Storage chooses only the capacity of the
single block that answers one request.

### 6.2 `release_tx(block)`

For a live descriptor previously returned by the same instance, release the
whole allocation corresponding to that exact descriptor. `{}` is a no-op.

The strategy must not reinterpret `capacity` as logical message length or wire
length. It is the payload-capacity grant originally reported for the block.

## 7. Shared behavioral obligations

Beyond the four signatures:

1. Successful live blocks remain valid until their matching release.
2. Simultaneously live blocks do not overlap.
3. Null/empty release is harmless.
4. Exhaustion is represented by null/empty acquisition, never an exception.
5. RX and TX quotas are independent. Holding every RX block must not prevent
   a TX acquisition that would otherwise succeed, and vice versa.
6. Acquire/release churn returns capacity; no successful block silently leaks.
7. Storage never mutates receiver-owned `RxBlock` metadata.
8. Release goes back to the exact storage instance that acquired the block.
9. Operations are `noexcept` and do not depend on exception propagation.
10. A detected invalid release never corrupts storage.

The generic engine does not ask for block counts, occupancy, allocation
statistics, a raw block size, or a payload span. Those may be strategy-specific
observers, but they are not part of `cobs::Storage`.

## 8. Built-in strategies

### 8.1 `cobs::Heap<WireFormat>`

Default:

```cpp
using DefaultFormat = cobs::Format<>; // Format<255, 255>
using DefaultStorage = cobs::Heap<>;
cobs::Endpoint<> endpoint;
```

The 255-byte default is the largest symmetric format that keeps the decoded
length field to one byte. Larger links opt into an explicit `Format`.

Properties:

- stateless and stored at no size cost where `[[no_unique_address]]` applies;
- RX allocation is exactly `sizeof(RxBlock) + requested_size`;
- TX allocation is exactly the format's physical size for the request;
- TX reports exactly the requested payload capacity;
- uses nothrow global allocation and explicit null failure;
- has no built-in quota or occupancy counters.

Choose Heap for desktop use, tests, or systems where dynamic allocation is an
accepted policy.

### 8.2 `cobs::Pool<RxBlocks, TxBlocks, WireFormat>`

Example:

```cpp
using Wire = cobs::Format<1024, 64>;
using Memory = cobs::Pool<8, 2, Wire>;
cobs::Endpoint<Memory> endpoint;
```

`WireFormat` defaults to `cobs::Format<>`, but the RX and TX block counts stay
explicit because they determine both static RAM use and backpressure:

```cpp
cobs::Endpoint<cobs::Pool<8, 2>> endpoint; // Format<255, 255>
```

Properties:

- owns separate fixed RX and TX block pools;
- O(1) acquire;
- deterministic capacity and no heap;
- every RX slab holds `sizeof(RxBlock) + max_receive_size`;
- every TX slab holds
  `Format::tx_storage_size_for_capacity(max_send_size)`;
- every accepted TX request reports `max_send_size`;
- exposes `rx_available()`, `tx_available()`, `rx_stats()`, and `tx_stats()`;
- counts pool exhaustion and rejected releases;
- keeps invalid/double-free checks enabled independently of `NDEBUG` unless
  explicitly compiled with `COBS_POOL_CHECKS=0`.

`RxBlocks` and `TxBlocks` must each be at least one; `detail::BlockPool`
rejects a zero-block configuration at compile time.

### 8.3 Sizing Pool

RX count is the maximum number of blocks concurrently:

- being decoded;
- waiting in the ready queue;
- retained by application `Packet` handles.

Copies of one packet do not consume new blocks, but they extend that block's
lifetime.

TX count is the maximum number of blocks concurrently:

- held by non-empty application `Message` objects;
- held as the endpoint's one active TX block.

If a pending queue remains full while a transfer is active, budget
`pending_depth + 1`. If sending removes the queue head and it is not replenished
until completion, the active block is that former head and no extra block is
needed.

An empty `make_message()` result is normal pool back-pressure and must be
handled.

## 9. Minimal custom strategy

This complete example deliberately provides exactly the checked surface. It
uses dynamic memory only to make the geometry visible; a real arena strategy
would replace the two allocation mechanisms without changing its public
contract.

```cpp
#include "Storage.h"

#include <cstddef>
#include <memory>
#include <new>

class ExampleStorage final {
public:
    using Format = cobs::Format<128, 64>;
    using RxBlock = cobs::RxBlock<ExampleStorage>;

    ExampleStorage() noexcept = default;
    ExampleStorage(const ExampleStorage&) = delete;
    ExampleStorage& operator=(const ExampleStorage&) = delete;

    [[nodiscard]] RxBlock* acquire_rx(std::size_t requested) noexcept
    {
        if (requested > Format::max_receive_size) {
            return nullptr;
        }

        void* const memory =
            ::operator new(sizeof(RxBlock) + requested, std::nothrow);
        if (memory == nullptr) {
            return nullptr;
        }
        return std::construct_at(static_cast<RxBlock*>(memory));
    }

    void release_rx(RxBlock* block) noexcept
    {
        if (block == nullptr) {
            return;
        }
        std::destroy_at(block);
        ::operator delete(static_cast<void*>(block));
    }

    [[nodiscard]] cobs::TxBlock acquire_tx(std::size_t requested) noexcept
    {
        if (requested > Format::max_send_size) {
            return {};
        }

        void* const memory = ::operator new(
            Format::tx_storage_size_for_capacity(requested),
            std::nothrow);
        if (memory == nullptr) {
            return {};
        }
        return {static_cast<std::byte*>(memory), requested};
    }

    void release_tx(cobs::TxBlock block) noexcept
    {
        ::operator delete(static_cast<void*>(block.memory));
    }
};

static_assert(cobs::Storage<ExampleStorage>);
```

Do not set `RxBlock::owner` or other private fields. The receiver does that.
Do not expose a second copy of the limits. `Format` does that.

Storage needing runtime configuration is constructed inside the endpoint:

```cpp
cobs::Endpoint<ExternalArenaStorage> endpoint{
    std::in_place,
    rx_region,
    rx_region_size,
    tx_region,
    tx_region_size
};
```

There is no endpoint constructor accepting a ready-made storage object. This
allows non-copyable/non-movable strategies and keeps one ownership route.

## 10. External and DMA-visible memory

The endpoint owning storage by value does not require the endpoint object
itself to reside in DMA-visible RAM. A custom strategy may store pointers or
spans to external regions.

Only the TX bytes read directly by DMA must satisfy that transport's placement,
alignment, cache, and lifetime requirements. RX blocks are written by the COBS
decoder on the CPU after bytes arrive from the transport's own RX buffers.

Those hardware rules are strategy/transport integration requirements, not
extra fields in `cobs::Storage`. The storage grant must nevertheless remain
valid for the entire active transport borrow.

## 11. Endpoint and block lifetimes

`Endpoint` owns its storage object. Therefore:

- it is neither copyable nor movable;
- it must outlive all messages and packets created from it;
- a packet's last release calls that exact embedded storage instance;
- a message from one endpoint cannot be sent through another endpoint of the
  same type;
- an endpoint must not be destroyed while its TX block is still borrowed.

Destroying an unsent `Message` releases its block. A successful send empties
the message without releasing; `Endpoint::poll()` releases after the busy
delegate reports false. Destroying a `Packet` releases only its reference; the
last reference releases the block.

## 12. Execution-domain requirements

The concept promises no thread or ISR safety. Built-in storage and ownership
metadata have no locks, and packet references are plain integers.

By default:

- serialize endpoint calls externally;
- keep packet copy/move/reset operations in one execution domain;
- reclaim transmitted storage through `poll()` in normal context;
- do not call storage concurrently unless the particular strategy adds and
  documents stronger guarantees.

A thread-safe custom allocator alone does not make `Endpoint` or `Packet`
thread-safe.

## 13. Conformance checklist

Before using a custom strategy:

### Compile-time

- [ ] Include only `Storage.h` for the extension surface.
- [ ] Name exactly one `Format`.
- [ ] Use `using RxBlock = cobs::RxBlock<ThisType>`.
- [ ] Implement all four operations with exact signatures and `noexcept`.
- [ ] `static_assert(cobs::Storage<ThisType>)` passes.
- [ ] Constructor used by `Endpoint` is `noexcept`.

The repository also compiles `compile_fail/storage_missing_tx.cpp` expecting
the top-level Endpoint contract diagnostic. It complements the positive
`static_assert` and prevents a partially implemented strategy from failing
only deep inside receiver/message templates.

### RX behavior

- [ ] Requests from zero through `max_receive_size` either fully succeed or
      return null.
- [ ] A successful region is aligned, constructed, contiguous, and large
      enough for header plus requested payload.
- [ ] Live acquisitions are distinct and non-overlapping.
- [ ] Release destroys the object and restores capacity.
- [ ] Null release is harmless.
- [ ] Storage never touches private receiver metadata.

### TX behavior

- [ ] Request zero returns real usable storage when capacity is available.
- [ ] Successful grant obeys
      `requested <= capacity <= max_send_size`.
- [ ] Physical bytes cover
      `Format::tx_storage_size_for_capacity(capacity)`.
- [ ] Over-limit request returns `{}`.
- [ ] Exact `TxBlock` descriptor is accepted on release.
- [ ] Empty release is harmless.

### System behavior

- [ ] RX exhaustion does not starve TX.
- [ ] TX exhaustion does not corrupt or consume RX capacity.
- [ ] Repeated acquire/release churn does not leak.
- [ ] Foreign/double release is rejected before destructive cleanup if the
      strategy claims validation.
- [ ] Endpoint, packets, messages, and any external region obey the lifetime
      contract.
- [ ] The shared storage suite and both `NDEBUG` runs pass.

The repository's `cobs/tests/test_storage.cpp` is the executable behavioral
specification. Strategy-specific tests belong beside it rather than inside the
generic engine.
