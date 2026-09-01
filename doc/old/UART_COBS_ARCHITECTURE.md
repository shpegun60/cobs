# UART + COBS Transport Architecture (historical sketch)

> **ARCHIVED — NOT A CURRENT API OR IMPLEMENTATION CONTRACT**
>
> This document records the original design exploration. Its examples use
> superseded names and models, including `get_msg()`, allocator-shaped
> operations, a virtual `IByteTx`, and an earlier monolithic COBS surface.
> Do not implement against them.
>
> Current sources of truth are
> [ARCHITECTURE.md](../ARCHITECTURE.md) for components and ownership,
> [PROTOCOL.md](../PROTOCOL.md) for wire behavior,
> [STORAGE.md](../STORAGE.md) for memory strategies, and
> [BUILD.md](../BUILD.md) for verification. Current UART behavior is defined
> by `uart/Uart.h` and its host/port tests. The text below is preserved as
> design history.

## 1. Goal

The goal is to build a reusable embedded communication stack where:

- `UART` is only a byte-stream transport.
- `COBS` owns framing, packet lifetime, RX/TX protocol state, and packet allocation.
- RX and TX have intentionally different ownership models.
- UART does not know anything about COBS, packet size, CRC, packet queues, or protocol semantics.
- No TX queue exists inside UART.
- The allocator is selected at compile time for COBS.
- The default allocator may use the heap.
- Embedded targets may provide a fixed-block allocator backed by static memory.
- RX packets may be retained by application code without copying.
- TX memory has exclusive ownership and no shared-reference overhead.
- The same COBS layer can later work above UART, TCP, USB CDC, or another byte-stream transport.

The intended user-facing API should stay small:

```cpp
auto msg = cobs.get_msg();

if (msg) {
    msg->write(header);
    msg->write(payload);
    msg->push();
}

while (auto packet = cobs.pop_packet()) {
    process(packet->data());
}
```

---

# 2. Layer separation

The architecture is split into three logical layers:

```text
+-----------------------------+
|         Application         |
|                             |
|  CobsMsg / PacketRef        |
+--------------+--------------+
               |
               v
+-----------------------------+
|            COBS             |
|                             |
| framing                     |
| RX stream state             |
| RX packet allocation        |
| TX frame allocation         |
| packet lifetime             |
| optional CRC                |
+--------------+--------------+
               |
               v
+-----------------------------+
|        Byte Transport       |
|                             |
| UART / TCP / USB CDC / ...  |
+-----------------------------+
```

The most important rule is:

> UART handles bytes. COBS handles packets.

---

# 3. UART responsibilities

UART should remain hardware-specific and intentionally simple.

It is responsible for:

- RX DMA / interrupt configuration.
- RX DMA buffers.
- RX buffer switching in interrupt context.
- RX SPSC queue.
- Calling the user RX handler from `proceed()`.
- Starting TX DMA for a contiguous byte span.
- Reporting `tx_busy()`.
- Clearing the TX busy state after DMA completion.

UART is **not** responsible for:

- COBS.
- CRC.
- packet framing.
- protocol packet size.
- packet allocation.
- RX packet lifetime.
- TX packet lifetime.
- TX retry policy.
- TX priority.
- a TX queue.

A minimal transport interface may look like:

```cpp
class IByteTx
{
public:
    virtual ~IByteTx() = default;

    [[nodiscard]]
    virtual bool tx_busy() const noexcept = 0;

    virtual bool send(
        std::span<const uint8_t> bytes) noexcept = 0;
};
```

UART implements this interface.

---

# 4. UART RX model

UART RX is asynchronous.

The hardware may produce arbitrary chunks depending on:

- DMA buffer size.
- DMA normal mode.
- DMA circular mode.
- UART IDLE detection.
- interrupt timing.
- sender pauses.

Therefore an RX callback does **not** mean "a protocol packet arrived".

It means only:

> These bytes became available.

For example, one COBS frame may arrive as:

```text
Chunk #1:
03 11 22

Chunk #2:
04 33 44

Chunk #3:
55 00
```

Or one UART chunk may contain several frames:

```text
03 11 22 00 04 33 44 55 00 02 AA 00
```

COBS must handle both cases.

---

# 5. UART RX buffering

The UART RX path uses its own small hardware-oriented buffers.

Example:

```text
4 x 256-byte UART RX buffers
```

These buffers exist because DMA requires memory that remains valid while hardware writes to it.

The interrupt handler should do as little work as possible:

```text
DMA/IDLE interrupt
      |
      v
mark completed buffer
      |
      v
push descriptor into SPSC
      |
      v
switch DMA to next free buffer
      |
      v
return from interrupt
```

No COBS parsing, allocation, CRC, or application callback should happen in the ISR.

`Uart::proceed()` consumes the SPSC queue and invokes the RX handler:

```cpp
void Uart::proceed()
{
    RxChunk chunk;

    while (rx_queue_.pop(chunk)) {
        if (rx_handler_) {
            rx_handler_(chunk.bytes);
        }

        release_rx_buffer(chunk.buffer_index);
    }
}
```

The span passed to the handler is valid only during the callback.

COBS must consume it immediately.

---

# 6. UART storage modes

UART may support two storage styles.

## 6.1 External storage

Useful when the user must control the exact memory location, for example on STM32H7:

```cpp
alignas(32)
__attribute__((section(".dma_rx")))
std::array<uint8_t, 256 * 8> rx_memory;

Uart uart{
    huart3,
    rx_memory,
    256
};
```

This is useful for:

- DMA-accessible RAM.
- MPU regions.
- non-cacheable RAM.
- cache-line alignment.
- linker-controlled memory placement.

## 6.2 Convenience static wrapper

A small template wrapper may own the storage locally:

```cpp
template<
    std::size_t ChunkSize,
    std::size_t ChunkCount>
class StaticUart
{
public:
    explicit StaticUart(UART_HandleTypeDef& handle)
        : uart_(handle, storage_, ChunkSize)
    {
    }

    Uart& get() noexcept
    {
        return uart_;
    }

private:
    std::array<uint8_t, ChunkSize * ChunkCount> storage_{};
    Uart uart_;
};
```

Usage:

```cpp
StaticUart<256, 4> uart{huart3};
```

The important design choice is that the actual UART engine remains non-template.

---

# 7. UART TX model

UART TX is intentionally much simpler than RX.

There is no TX queue inside UART.

The API is essentially:

```cpp
bool tx_busy() const noexcept;

bool send(
    std::span<const uint8_t> bytes) noexcept;
```

If COBS gives UART 100 bytes:

```cpp
uart.send(frame);
```

UART simply starts DMA for those 100 bytes.

It does not split the message into "UART packets".

UART does not care whether the data is:

- COBS.
- Modbus.
- protobuf.
- a JPEG.
- random binary data.

It only transmits a contiguous byte range.

---

# 8. Why UART has no TX queue

TX policy does not belong to UART.

Different applications may want different behavior when TX is busy:

- retry later.
- drop telemetry.
- replace old telemetry with newer telemetry.
- maintain a FIFO.
- maintain priorities.
- block.
- return an error.

UART should not decide this.

Therefore:

```text
UART TX state:
    tx_busy
    current DMA transfer

No UART TX scheduler.
No UART TX queue.
```

Higher layers decide what to do on `Busy`.

---

# 9. COBS responsibilities

COBS owns all protocol-level behavior.

It is responsible for:

- consuming arbitrary byte chunks.
- maintaining incremental RX decoding state.
- detecting the `0x00` delimiter.
- reconstructing decoded packets.
- rejecting malformed frames.
- handling overflow/drop mode.
- allocating RX packet memory.
- retaining completed RX packets.
- allocating TX message memory.
- COBS encoding.
- appending the final `0x00` delimiter.
- optional CRC policy.
- keeping TX memory alive until UART DMA completes.
- releasing memory through the configured allocator.

UART remains unaware of all of this.

---

# 10. COBS allocator as a template parameter

COBS is templated by allocator:

```cpp
template<
    class Allocator = HeapAllocator>
class Cobs
{
    // ...
};
```

This gives two useful modes.

## Default mode

```cpp
Cobs cobs{
    uart,
    1024
};
```

The default allocator may use normal heap allocation.

## Embedded fixed-pool mode

```cpp
FixedPoolAllocator allocator{
    storage,
    block_size
};

Cobs<FixedPoolAllocator> cobs{
    uart,
    1024,
    allocator
};
```

Benefits:

- no virtual allocator calls.
- allocator operations may inline.
- the allocator type is known at compile time.
- fixed-pool allocation may be O(1).
- no generic heap fragmentation.
- application-facing API remains unchanged.

---

# 11. Fixed-block allocator

For this protocol, a fixed-block allocator is a natural fit because the maximum packet size is known.

Example:

```text
Max raw payload:      1024 bytes
COBS overhead:        a few bytes
Delimiter:            1 byte
Metadata:             small fixed header

Allocator block:      e.g. 1056 or 1088 bytes
Block count:          e.g. 8
```

The pool consists of equal-size blocks:

```text
+----------+
| Block 0  |
+----------+
| Block 1  |
+----------+
| Block 2  |
+----------+
| Block 3  |
+----------+
```

Allocation may simply find/pop a free block.

Deallocation can derive the block index from the pointer:

```cpp
const auto offset =
    static_cast<std::byte*>(ptr) - storage_.data();

const auto index =
    static_cast<std::size_t>(offset) / block_size_;

used_[index] = false;
```

The allocator already knows:

- pool base address.
- block size.
- block count.

Therefore deallocation only needs the pointer.

---

# 12. RX ownership

RX is the interesting side because packet lifetime is not known by COBS.

Application code may do:

```cpp
auto packet = cobs.pop_packet();

parser_queue.push(packet);
logger_queue.push(packet);
saved_packet = packet;
```

The packet may outlive:

- the current loop iteration.
- the COBS ready queue.
- the parser.
- one specific application component.

Therefore RX needs shared ownership semantics.

However, `std::shared_ptr` is more general than necessary and usually carries:

- a separate control block.
- strong reference count.
- weak reference count.
- type-erased deleter.
- generic thread-safety machinery.
- typically two pointers in the handle.

For this architecture, a custom intrusive `PacketRef` is preferred.

---

# 13. PacketRef

`PacketRef` is a narrow, embedded-oriented shared ownership handle.

The reference count lives directly inside `RxPacket`.

Example:

```cpp
struct RxPacket
{
    uint16_t refs = 1;
    uint16_t size = 0;

    // Payload storage follows or belongs to the same allocator block.
};
```

A minimal handle:

```cpp
class PacketRef
{
public:
    PacketRef() noexcept = default;

    PacketRef(const PacketRef& other) noexcept
        : ptr_(other.ptr_)
    {
        retain();
    }

    PacketRef(PacketRef&& other) noexcept
        : ptr_(std::exchange(other.ptr_, nullptr))
    {
    }

    PacketRef& operator=(const PacketRef& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        reset();
        ptr_ = other.ptr_;
        retain();

        return *this;
    }

    PacketRef& operator=(PacketRef&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        reset();
        ptr_ = std::exchange(other.ptr_, nullptr);

        return *this;
    }

    ~PacketRef()
    {
        reset();
    }

    explicit operator bool() const noexcept
    {
        return ptr_ != nullptr;
    }

    const RxPacket* operator->() const noexcept
    {
        return ptr_;
    }

    const RxPacket& operator*() const noexcept
    {
        return *ptr_;
    }

    void reset() noexcept
    {
        if (!ptr_) {
            return;
        }

        auto* packet = ptr_;
        ptr_ = nullptr;

        if (--packet->refs == 0) {
            release_packet(packet);
        }
    }

private:
    void retain() noexcept
    {
        if (ptr_) {
            ++ptr_->refs;
        }
    }

    RxPacket* ptr_ = nullptr;
};
```

The exact implementation of `release_packet()` depends on how allocator ownership is encoded.

The important semantic behavior is:

```text
copy PacketRef:
    refs++

move PacketRef:
    pointer transfer only

destroy PacketRef:
    refs--

last PacketRef:
    return block to allocator
```

---

# 14. PacketRef vs std::shared_ptr

For this specific use case, `PacketRef` keeps the important functionality:

- copyable ownership.
- movable ownership.
- automatic release.
- `operator bool`.
- `operator->`.
- `operator*`.
- `reset()`.
- packet may be retained by arbitrary application code.

Features intentionally not required:

- `weak_ptr`.
- aliasing constructors.
- `enable_shared_from_this`.
- pointer casting helpers.
- arbitrary per-object deleters.
- generic type-erased ownership models.

This is a deliberate trade:

> Keep the shared lifetime semantics we actually use, remove the general-purpose machinery we do not need.

---

# 15. RX packet immutability

Once COBS finishes decoding a frame, application code should only receive read-only access.

For example:

```cpp
class RxPacket
{
public:
    [[nodiscard]]
    std::span<const uint8_t> data() const noexcept;
};
```

COBS may write during construction/decoding.

After completion:

```text
COBS writes -> frame complete -> packet becomes read-only
```

This allows multiple tasks/components to read the same packet without copying.

---

# 16. Multi-task / RTOS PacketRef

If all `PacketRef` copies/releases happen in one execution context, a plain integer reference count is enough:

```cpp
uint16_t refs;
```

This is the fastest option.

If references can be copied or destroyed concurrently by multiple RTOS tasks, the reference count must be synchronized.

For example:

```cpp
std::atomic<uint32_t> refs{1};
```

Copy:

```cpp
refs.fetch_add(
    1,
    std::memory_order_relaxed);
```

Release:

```cpp
if (refs.fetch_sub(
        1,
        std::memory_order_acq_rel) == 1) {
    release_packet();
}
```

The payload itself still does not need a mutex if it is immutable and only read by the tasks.

A future implementation may expose this as a policy:

```text
BareMetalRefCounter
AtomicRefCounter
```

without changing the application-facing `PacketRef` behavior.

---

# 17. RX pipeline

The full RX path is:

```text
UART hardware
     |
     v
DMA RX buffer
     |
     | ISR
     v
RX SPSC descriptor queue
     |
     | Uart::proceed()
     v
RxHandler(span<const uint8_t>)
     |
     v
Cobs::consume()
     |
     v
incremental COBS decoder
     |
     v
allocator.allocate()
     |
     v
RxPacket
     |
     | frame completed
     v
COBS ready packet queue
     |
     v
Cobs::pop_packet()
     |
     v
PacketRef
     |
     v
Application
     |
     | last reference disappears
     v
allocator.deallocate()
```

---

# 18. RX drop/resynchronization behavior

COBS gives a natural resynchronization point:

```text
0x00
```

If:

- a UART chunk is lost.
- the packet is too large.
- no allocator block is available.
- decoding becomes invalid.

COBS may enter a drop state:

```text
discard bytes
    |
    v
wait for next 0x00
    |
    v
reset decoder
    |
    v
start next frame
```

This is one of the major advantages of COBS framing for byte streams.

---

# 19. TX ownership

TX does not need shared ownership.

There is a single logical owner at every moment:

```text
CobsMsg
   |
   v
Cobs active TX
   |
   v
UART DMA borrows the span
   |
   v
TX complete
   |
   v
memory released
```

Therefore TX should use exclusive ownership.

Internally this may be represented with:

```cpp
std::unique_ptr<TxFrame, TxDeleter>
```

The deleter is an implementation detail derived from the allocator type.

The user does not need to see this type directly.

---

# 20. CobsMsg

`CobsMsg` is the user-facing TX builder.

Usage:

```cpp
auto msg = cobs.get_msg();

if (!msg) {
    return;
}

msg->write(command);
msg->write(address);

auto payload = msg->reserve(64);

fill_payload(payload);

const auto result = msg->push();
```

`CobsMsg` owns its TX allocation until a successful `push()` transfers ownership to COBS.

Conceptually:

```text
get_msg()
   |
   v
allocator.allocate()
   |
   v
CobsMsg owns TxFrame
   |
   v
user fills payload
   |
   v
push()
```

If `push()` returns `Busy`, the message still owns its memory and may be retried.

---

# 21. TX push flow

On successful `push()`:

```text
CobsMsg raw payload
      |
      v
optional CRC
      |
      v
COBS encode
      |
      v
append 0x00
      |
      v
transport.send(span)
      |
      v
Cobs active_tx owns memory
      |
      v
UART DMA reads memory
```

UART only borrows the span.

The allocation remains owned by COBS until TX finishes.

---

# 22. COBS TX completion

COBS may determine completion by checking:

```cpp
transport.tx_busy()
```

Example:

```cpp
void Cobs::proceed()
{
    if (active_tx_ && !transport_.tx_busy()) {
        active_tx_.reset();
    }
}
```

Resetting the internal exclusive owner returns the block to the allocator.

A future transport may expose an explicit TX-complete notification instead.

---

# 23. TX Busy behavior

UART contains no queue.

Therefore:

```cpp
auto result = msg->push();
```

may return:

```cpp
enum class SendResult
{
    Sent,
    Busy,
    Invalid,
    Error
};
```

If busy:

```cpp
if (msg->push() == SendResult::Busy) {
    pending = std::move(*msg);
}
```

The higher layer decides whether to:

- retry.
- drop.
- queue.
- prioritize.
- replace.

This keeps UART policy-free.

---

# 24. Zero-copy TX goal

The ideal TX layout uses one allocator block.

Before finalization:

```text
+---------------+----------------------------------+
| COBS headroom | raw payload                      |
+---------------+----------------------------------+
                ^
                CobsMsg writes here
```

After finalization:

```text
+--------------------------------------------------+
| COBS encoded wire frame                    | 00 |
+--------------------------------------------------+
^
UART DMA reads here
```

This avoids:

```text
raw buffer -> encoded buffer -> UART buffer
```

and instead aims for:

```text
one allocator block -> UART DMA
```

The chosen COBS encoder must explicitly support the required overlapping/in-place layout.

If not, a separate temporary encoding strategy is required.

Correctness is more important than forcing zero-copy.

---

# 25. TX pipeline

The full TX path is:

```text
Application
     |
     v
Cobs::get_msg()
     |
     v
allocator.allocate()
     |
     v
CobsMsg
     |
     v
write / reserve
     |
     v
push()
     |
     v
COBS finalize
     |
     v
Cobs active TX owner
     |
     v
UART::send(span)
     |
     v
DMA
     |
     v
UART tx_busy = false
     |
     v
Cobs::proceed()
     |
     v
allocation released
```

---

# 26. Suggested COBS skeleton

The exact implementation is still open, but the public shape should stay close to:

```cpp
template<class Allocator = HeapAllocator>
class Cobs
{
public:
    explicit Cobs(
        IByteTx& transport,
        std::size_t max_packet_size,
        Allocator allocator = {});

    void consume(
        std::span<const uint8_t> bytes);

    void proceed();

    [[nodiscard]]
    std::optional<CobsMsg> get_msg();

    [[nodiscard]]
    PacketRef pop_packet();

private:
    IByteTx* transport_;
    Allocator allocator_;

    std::size_t max_packet_size_;

    // RX stream state
    // RX current packet
    // RX ready queue

    // TX active exclusive frame
};
```

---

# 27. Suggested user setup

## Heap/default allocator

```cpp
StaticUart<256, 4> uart{huart3};

Cobs cobs{
    uart.get(),
    1024
};

uart.get().set_rx_handler(
    [&](std::span<const uint8_t> bytes) {
        cobs.consume(bytes);
    });
```

Main loop:

```cpp
void loop()
{
    uart.get().proceed();
    cobs.proceed();

    while (auto packet = cobs.pop_packet()) {
        process(packet->data());
    }
}
```

TX:

```cpp
void send_status()
{
    auto msg = cobs.get_msg();

    if (!msg) {
        return;
    }

    msg->write(status_header);

    auto payload = msg->reserve(payload_size);
    build_status(payload);

    const auto result = msg->push();

    if (result == SendResult::Busy) {
        // Application decides what to do.
    }
}
```

---

# 28. Suggested embedded fixed-pool setup

```cpp
constexpr std::size_t kPacketBlockSize = 1088;
constexpr std::size_t kPacketBlockCount = 8;

alignas(32)
std::array<
    std::byte,
    kPacketBlockSize * kPacketBlockCount
> protocol_memory;

FixedPoolAllocator allocator{
    protocol_memory,
    kPacketBlockSize
};

StaticUart<256, 4> uart{huart3};

Cobs<FixedPoolAllocator> cobs{
    uart.get(),
    1024,
    allocator
};
```

The rest of the application remains the same.

That is an important design goal:

> Changing the allocator must not change the normal message API.

---

# 29. Why allocator belongs to COBS, not UART

UART buffer requirements are hardware-specific:

```text
small DMA chunks
alignment
DMA-accessible RAM
cache concerns
```

COBS packet requirements are protocol-specific:

```text
maximum packet size
packet lifetime
RX retention
TX frame lifetime
```

These are different concerns.

Therefore:

```text
UART memory:
    hardware RX buffering

COBS memory:
    logical packet buffering
```

This separation is worth a memory copy if one ever becomes necessary.

For TX, the planned architecture can avoid the copy entirely because UART DMA may read directly from COBS-owned memory.

---

# 30. Why RX and TX are intentionally asymmetric

Trying to make RX and TX structurally identical would make the design worse.

RX:

```text
hardware produces bytes whenever it wants
packet lifetime may become shared
requires incremental decoder state
```

TX:

```text
application explicitly creates message
single owner
known lifetime:
create -> send -> DMA complete -> free
```

Therefore:

```text
RX ownership:
    PacketRef
    shared/intrusive lifetime

TX ownership:
    exclusive
    internal unique_ptr or equivalent
```

This asymmetry reflects the actual data flow.

---

# 31. Optional CRC

COBS framing and CRC solve different problems.

COBS provides:

```text
packet boundary
resynchronization
```

CRC provides:

```text
packet integrity validation
```

A UART configuration may use:

```text
payload -> CRC -> COBS -> 0x00
```

A reliable transport such as TCP may choose:

```text
payload -> COBS -> 0x00
```

without an additional CRC.

CRC should therefore be a COBS/protocol policy, not a UART feature.

---

# 32. Future transport reuse

Because COBS depends only on a byte transport interface, the same layer can later work above:

```text
UART
TCP
USB CDC
Bluetooth stream
file replay
unit-test byte source
```

RX:

```cpp
cobs.consume(bytes);
```

TX:

```cpp
transport.send(frame);
```

The packet-facing application API remains unchanged.

---

# 33. Key invariants

The implementation should preserve these invariants.

### UART

1. UART does not understand protocol packets.
2. RX callback data is only a byte chunk.
3. RX chunk lifetime ends after the callback.
4. UART has no TX packet queue.
5. UART TX only borrows memory while DMA is active.
6. `tx_busy()` accurately reflects whether that borrowed memory is still in use.

### COBS RX

1. COBS owns RX stream state.
2. Completed packets never depend on UART RX buffers.
3. Completed packets are immutable to application code.
4. Packet memory remains alive while at least one `PacketRef` exists.
5. Invalid or oversized frames are dropped until the next delimiter.

### COBS TX

1. `CobsMsg` exclusively owns its allocation before successful `push()`.
2. `Busy` does not destroy the message.
3. Successful `push()` transfers ownership to COBS.
4. COBS retains TX memory while UART DMA is active.
5. COBS releases TX memory only after TX completion.

### Allocator

1. COBS does not care whether memory comes from heap or static RAM.
2. Fixed-pool allocation must be deterministic.
3. Deallocation must be possible from the allocated pointer.
4. Allocator lifetime must outlive all objects allocated from it.

---

# 34. Error cases to expose

Useful counters/status values should include:

```text
UART:
    rx_overrun
    rx_no_free_dma_buffer
    tx_start_error

COBS RX:
    malformed_frame
    oversized_frame
    allocation_failure
    dropped_frame

COBS TX:
    allocation_failure
    busy
    encode_error
    transport_error
```

These should be observable but should not complicate the normal API.

---

# 35. Final architecture summary

```text
                          RX

UART peripheral
      |
      v
DMA hardware buffers
      |
      v
RX SPSC
      |
      v
Uart::proceed()
      |
      v
Rx delegate(span<const byte>)
      |
      v
Cobs::consume()
      |
      v
incremental decoder
      |
      v
Allocator -> RxPacket
      |
      v
ready packet queue
      |
      v
PacketRef
      |
      v
Application
      |
      v
last PacketRef
      |
      v
Allocator release


                          TX

Application
      |
      v
Cobs::get_msg()
      |
      v
Allocator -> CobsMsg / TxFrame
      |
      v
write / reserve
      |
      v
push()
      |
      v
CRC optional
      |
      v
COBS encode + 0x00
      |
      v
Cobs owns active TX frame
      |
      v
Uart::send(span)
      |
      v
DMA
      |
      v
tx_busy == false
      |
      v
Cobs::proceed()
      |
      v
Allocator release
```

The core philosophy is:

> UART is a byte-stream driver.  
> COBS is a packet transport layer.  
> The allocator owns memory policy.  
> RX uses shared intrusive lifetime through `PacketRef`.  
> TX uses exclusive ownership.  
> Application code sees packets and messages, not DMA buffers.

This keeps hardware concerns, framing concerns, memory policy, and application ownership separated without turning the library into template archaeology.
