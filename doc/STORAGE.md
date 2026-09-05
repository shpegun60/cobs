<!--
Author: shpegun60
SPDX-License-Identifier: MIT
-->

# Shared storage contract

This is the canonical extension guide for both COBS and Modbus RTU.
Include `wire/Storage.h` to write storage; include `Cobs.h` or
`modbus/rtu/Rtu.h` to use an endpoint. No protocol-specific storage aliases
or forwarding headers are provided.

## Configuration and ownership

```cpp
using Memory = wire::Pool<8, 2>;
using Cobs = cobs::Endpoint<Memory, cobs::Format<>>;
using Rtu = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>>;
```

The first argument is a **memory specification**, not a pre-sized allocator.
It provides `template<class Geometry> class For`. Each endpoint computes its
own Geometry and constructs `Memory::For<Geometry>` in place. That concrete
type is available as `Endpoint::Storage`; the specification as
`Endpoint::Memory`. Two endpoints own separate instances, even if those
instances have the same type. Sharing an external arena is an explicit
custom-storage decision.

Storage knows nothing about Format, CRC, packet fields, parsing, queues,
growth, or transport. It supplies raw bytes and takes them back. Only the
protocol constructs its own RX metadata and stamps the typed owner pointer.
Only Packet manages references. There is no virtual dispatch or per-packet
type-erased deleter.

## Three compile-time geometry values

| Value | Meaning |
|---|---|
| `Geometry::rx_block_bytes` | Alignment-rounded upper bound on the largest RX request; safe minimum slot stride |
| `Geometry::tx_block_bytes` | Largest physical TX request the protocol can issue |
| `Geometry::alignment` | Minimum alignment of every returned RX pointer |

These are positive integral constant expressions; alignment is a power of
two and RX bytes are a multiple of alignment. `wire::Geometry<G>` checks
these properties, rejecting runtime sizes and unrounded RX rows.

There is no minimum: each request already supplies the exact size needed
now. Protocol minimum frame lengths belong to receiver validation.

Geometry is a `wire::BlockGeometry<RxBytes, TxBytes, Alignment>` keyed only
on those numbers. It is not nested in the algorithm-specific Endpoint type.
Format's Layout contains only structural values. Switching CRC16 Bitwise to
Table with unchanged limits preserves Layout, Geometry, concrete Storage,
Packet and Message types within each protocol.

```cpp
using G = Cobs::Geometry;
static_assert(G::rx_block_bytes <= MY_RX_BYTES);
static_assert(G::tx_block_bytes <= MY_TX_BYTES);
```

A smaller custom storage is also legal: it can reject large requests with
null/empty results. These assertions express an application's guarantee of
capacity, not an unconditional library requirement.

## Four operations, physical bytes only

```cpp
std::byte*    acquire_rx(std::size_t bytes) noexcept;
void          release_rx(std::byte* memory) noexcept;
wire::TxBlock acquire_tx(std::size_t bytes) noexcept;
void          release_tx(wire::TxBlock block) noexcept;
```

The descriptor is exactly:

```cpp
struct TxBlock {
    std::byte* memory = nullptr;
    std::size_t granted = 0;
};
```

`granted` is the physical writable allocation size, **not** useful payload
capacity, current message size, or wire-frame length.

RX success returns at least `bytes` aligned writable contiguous bytes, or
null on refusal. There is no RX capacity return; lying about available bytes
is a memory-safety bug. Each real RX request includes the protocol's private
block header and body. Heap requests remain exact even though the Geometry
maximum is rounded for static slots.

TX success returns a nonnull pointer and `granted >= bytes`, or `{}`.
A size class may grant more than `Geometry::tx_block_bytes`: Geometry limits
requests, not grants. Endpoint never enlarges a protocol's useful capacity
because storage happens to return more memory.

The exact descriptor travels unchanged:

```text
Storage -> Message -> Endpoint active TX -> same Storage instance
```

A Message defensively rejects a nonempty undersized grant, returns that
descriptor, and preserves its previous size, capacity and bytes. Useful
capacity is calculated independently and clamped to the Format limit.
COBS caches its exact inverse geometry once per allocation/growth; it does
not divide or search on each append. RTU subtracts its envelope from a
ceiling-clamped grant.

Growth acquires first, copies only existing payload, then releases the old
block. COBS payload offsets can differ between capacities. Finalized messages
do not grow or recompute CRC when a transport start is retried.

## Behavioral obligations

A concept checks syntax, not these runtime guarantees:

- live RX/TX allocations do not overlap and remain valid until released;
- release returns to the originating instance, with the original descriptor;
- null/empty release is harmless;
- allocation failure uses null/empty, never an exception;
- release is allocation-free and all four operations are noexcept;
- storage does not inspect or modify live protocol metadata;
- if independent RX/TX quotas are promised, exhaustion in one cannot starve
  the other; the built-in Pool provides that guarantee;
- a strategy claiming invalid-release checking rejects foreign/double release
  before touching the free list or unrelated memory.

`wire::ByteStorage<S>` checks the four exact signatures.
`wire::Storage<Memory, G>` checks Geometry and the bound `Memory::For<G>`.

Endpoint constructors have conditional noexcept based on Storage/CRC
construction. A throwing constructor is not mislabeled noexcept; hot-path
acquire/release/calculate/codec operations must still be nonthrowing.

## Correct slot alignment

Do not merely align the start of an array whose independently chosen row
size is not a multiple of alignment. Carry alignment in the element type:

```cpp
template<class G>
struct Slots {
    static_assert(MY_RX_BYTES >= G::rx_block_bytes);
    struct alignas(G::alignment) RxSlot {
        std::byte bytes[MY_RX_BYTES];
    };
    RxSlot rx[RX_COUNT];
    std::byte tx[TX_COUNT][MY_TX_BYTES];
};
```

The compiler rounds sizeof(RxSlot) so **every** slot is aligned. An array
using exactly `G::rx_block_bytes` as row size is also safe when its beginning
is aligned, because that number is already rounded. TX protocol alignment is
one; a DMA transport may impose stronger alignment and placement.

Packet buffers are CPU-owned. UART DMA still writes its own cache-aligned
chunks; COBS decodes into packet storage and RTU copies a validated candidate.
A custom packet allocator does not make UART DMA write into those packets.
TX bytes borrowed directly by DMA must satisfy that transport's requirements.

## Built-in memory specifications

`wire::Heap` is stateless. Its bound For uses nothrow global allocation,
grants exactly requested physical bytes, has no quota/occupancy counters and
rejects requests above Geometry. It supports only alignment guaranteed by
ordinary operator new; stronger alignment needs a custom strategy or Pool.

`wire::Pool<RxBlocks, TxBlocks>` owns two independent fixed-block pools.
Both quotas must be nonzero and stay explicit. Each TX acquisition grants
the whole `G::tx_block_bytes` slab. RX slabs use the rounded Geometry size.
Its underlying free-list slot may contain additional padding for pointer
alignment; use sizeof(Endpoint::Storage) or sizeof(Endpoint) for a RAM budget,
not merely the sum of requested bytes.

Pool exposes const `rx_available()`, `tx_available()`, `rx_stats()`,
`tx_stats()` through `endpoint.storage()`. Checks for foreign/duplicate
release remain enabled under NDEBUG. The opt-out macro is
`WIRE_POOL_CHECKS=0`, consistently defined in every translation unit.

RX quotas count building, queued and retained packets. Copies of a Packet
extend its block's lifetime without allocating another block. TX quotas count
live Messages plus the one active transport borrow. Exhaustion is normal
backpressure and must be handled by the application.

## Complete custom specification example

This wrapper is deliberately protocol-blind. Replace its internal Heap with
your own arena without changing the four operations or the endpoint APIs.

```cpp
#include "wire/Storage.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"

struct Requests { std::size_t rx = 0, tx = 0; };

struct ObservedMemory {
    template<class G>
    class For {
        wire::Heap::For<G> heap_;
        Requests& requests_;
    public:
        explicit For(Requests& r) noexcept : requests_(r) {}

        std::byte* acquire_rx(std::size_t bytes) noexcept {
            ++requests_.rx;
            return heap_.acquire_rx(bytes);
        }
        void release_rx(std::byte* p) noexcept { heap_.release_rx(p); }

        wire::TxBlock acquire_tx(std::size_t bytes) noexcept {
            ++requests_.tx;
            return heap_.acquire_tx(bytes);
        }
        void release_tx(wire::TxBlock block) noexcept {
            heap_.release_tx(block);
        }
    };
};

using Cobs = cobs::Endpoint<ObservedMemory>;
using Rtu = modbus::rtu::Endpoint<ObservedMemory>;
static_assert(wire::Storage<ObservedMemory, Cobs::Geometry>);
static_assert(wire::Storage<ObservedMemory, Rtu::Geometry>);

Requests cobs_requests, rtu_requests;
Cobs cobs_link{std::in_place, cobs_requests};
Rtu rtu_link{std::in_place, rtu_requests};
```

For a custom CRC and runtime memory arguments, both endpoints also accept
`Link{MyCrc{handle}, std::in_place, arena_arguments...}`.
Storage itself need not be copyable or movable.

## Lifetime and execution

Endpoint must outlive all issued Messages and Packets and every active
transport borrow. It cannot be copied or moved. A foreign Message is refused
even by another endpoint with the same type. Destroying an unsent Message
releases its allocation; Sent empties it and poll releases after busy=false.

Built-in storage, endpoint state and packet reference counts are not atomic.
Keep mutation/copy/release in one externally serialized execution domain.
A thread-safe allocator alone does not make Packet or Endpoint thread-safe.

## Conformance evidence

Run `sh wire/tests/run.sh`. It covers raw BlockPool and storage behavior
under checked/sanitized and NDEBUG builds, real COBS/RTU geometries, alignment
of every acquired RX slot, independent quotas and release checking.

`wire/tests/test_protocol_storage.cpp` passes the same custom specification
through both public endpoints. It checks excess grants beyond the maximum,
undersized grants at construction and growth, strong failure, payload moves,
retained packet references, transport borrowing and exact descriptor return.
Adapt the generic contract body in `wire/tests/test_storage.cpp` to your
storage and run it with each required Endpoint::Geometry.
