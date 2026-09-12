/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Reuse the frozen diagnostic's real-heap exhaustion, allocation-free UART
// logger and forwarding abort observers. Keep its measured source unchanged.
#define bench_init baseline_bench_init
#define bench_loop baseline_bench_loop
#include "../oom/bench.cpp"
#undef bench_init
#undef bench_loop
#include <malloc.h>

namespace {
namespace frame = modbus::rtu::framing;
struct PrivateFramer : frame::Standard<frame::Direction::Request> {
    static constexpr frame::Layout layout(frame::Direction d, uint8_t f) noexcept
    { return f == 0x41u ? frame::Layout::length_prefixed(2u) : frame::standard_layout(d, f); }
};
using Framed = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>, PrivateFramer>;
Framed framed_heap;
using BareGeometry = wire::BlockGeometry<128u, 128u, alignof(std::max_align_t)>;
wire::Heap::For<BareGeometry> bare_heap;
constexpr std::array<uint8_t, 2> prefix{0x41u, 0x42u};
constexpr std::array<uint8_t, 32> tail{0x11u, 0x22u, 0x33u, 0x44u};

struct Capture {
    std::array<uint8_t, 512u> buffer{};
    std::size_t size = 0u;
    bool held = false;
    bool send(std::span<const uint8_t> bytes) noexcept
    {
        if (held || bytes.size() > buffer.size()) { return false; }
        std::copy(bytes.begin(), bytes.end(), buffer.begin()); size = bytes.size(); held = true; return true;
    }
    bool busy() const noexcept { return held; }
    std::span<const uint8_t> bytes() const noexcept { return {buffer.data(), size}; }
} capture;

template<class E> auto make(E& e, std::size_t hint) noexcept
{
    if constexpr (requires { e.make_message(hint); }) { return e.make_message(hint); }
    else if constexpr (E::framed) { return e.make_message(0x11u, 0x41u, hint + 2u); }
    else { return e.make_message(0x11u, 3u, hint); }
}
template<class E> void ingest(E& e, std::span<const uint8_t> data) noexcept
{
    if constexpr (requires { e.consume(data); }) { e.consume(data); }
    else { e.receive_adu(data); }
}
template<class E> std::span<const uint8_t> application(const typename E::Packet& packet) noexcept
{
    if constexpr (std::is_same_v<E, Framed>) {
        auto data = packet.data();
        if (data.size() < 2u || ((static_cast<std::size_t>(data[0]) << 8u) | data[1]) != data.size() - 2u) {
            ++failures; return {};
        }
        return data.subspan(2u);
    } else { return packet.data(); }
}
void dump_wire(uint32_t protocol, std::size_t payload_size) noexcept
{
    emit("WIRE,"); const char tag[]{active, '\0'}; emit(tag); field(protocol); field(static_cast<uint32_t>(payload_size)); emit(",");
    constexpr char hex[] = "0123456789abcdef";
    for (auto b : capture.bytes()) { const char pair[]{hex[b >> 4u], hex[b & 15u], '\0'}; emit(pair); }
    emit("\n");
}
template<class E> void bind_capture(E& e) noexcept
{
    demand(e.bind(typename E::Sender{tiny::bind<&Capture::send>(capture)},
                  typename E::BusyQuery{tiny::bind<&Capture::busy>(capture)}));
}
template<class E> void send_and_check(E& e, typename E::Message& message, uint32_t protocol, bool grown = false) noexcept
{
    demand(e.send(message) == wire::SendResult::Sent && !message && e.tx_active());
    dump_wire(protocol, grown ? 34u : 2u);
    ingest(e, capture.bytes());
    auto packet = e.pop_packet();
    demand(static_cast<bool>(packet));
    if (packet) {
        auto data = application<E>(packet);
        demand(data.size() == (grown ? 34u : 2u));
        if (data.size() >= 2u) {
            demand(std::ranges::equal(data.first(2u), prefix));
            if (grown) { demand(std::ranges::equal(data.subspan(2u), tail)); }
        }
    }
    auto retained = packet; packet.reset();
    capture.held = false; e.poll(0u);
    demand(!e.tx_active() && !e.has_packet());
    retained.reset();
}
template<class E> void roundtrip(E& e, uint32_t protocol) noexcept
{
    bind_capture(e);
    auto message = make(e, 2u);
    demand(message && message.append_bytes(prefix));
    send_and_check(e, message, protocol);
}
void recovered(const std::size_t live_before) noexcept
{
    roundtrip(cobs_heap, 0u); roundtrip(rtu_heap, 1u); roundtrip(framed_heap, 2u);
    demand(held_count == 0u && mallinfo().uordblks == live_before);
    report("RECOVERED");
}
template<class E> void create_failure(E& e, const std::size_t live_before) noexcept
{
    exhaust(); report("BEFORE");
    auto message = make(e, 32u);
    demand(!message);
    report("REFUSED", 1u);
    release_all(); recovered(live_before);
}
template<class E> void growth_failure(E& e, uint32_t protocol, const std::size_t live_before) noexcept
{
    bind_capture(e);
    auto message = make(e, 2u);
    demand(message && message.append_bytes(prefix));
    const auto size = message.size(), capacity = message.capacity();
    demand(capacity - size < 4u);
    exhaust(); report("BEFORE", static_cast<uint32_t>(capacity));
    demand(!message.reserve(capacity + 32u));
    demand(!message.append_bytes(tail));
    demand(!message.append_native(uint32_t{0xA1B2C3D4u}));
    demand(!message.append_be(uint32_t{0xA1B2C3D4u}));
    demand(!message.append_le(uint32_t{0xA1B2C3D4u}));
    demand(message && message.size() == size && message.capacity() == capacity);
    report("REFUSED", 5u);
    release_all();
    // Retry the SAME live message, then validate the preserved prefix on wire.
    demand(message.append_bytes(tail));
    send_and_check(e, message, protocol, true);
    recovered(live_before);
}
template<class E> void rx_failure(E& e, uint32_t protocol, const std::size_t live_before) noexcept
{
    roundtrip(e, protocol); // leaves the independent complete wire vector in capture
    ingest(e, capture.bytes()); auto original = e.pop_packet(); auto retained = original;
    demand(static_cast<bool>(original));
    const auto before = e.stats().rx;
    exhaust(); report("BEFORE");
    for (unsigned i = 0u; i < 3u; ++i) { ingest(e, capture.bytes()); }
    const auto after = e.stats().rx;
    demand(!e.has_packet() && after.allocation_failure == before.allocation_failure + 3u &&
           after.frames_received == before.frames_received && after.crc_errors == before.crc_errors);
    if (retained) { demand(std::ranges::equal(application<E>(retained), prefix)); }
    else { ++failures; }
    report("REFUSED", 3u);
    release_all();
    ingest(e, capture.bytes()); auto fresh = e.pop_packet();
    demand(fresh && std::ranges::equal(application<E>(fresh), prefix));
    demand(e.stats().rx.frames_received == before.frames_received + 1u);
    original.reset(); retained.reset(); fresh.reset();
    recovered(live_before);
}
} // namespace

// Non-inlined, live-executed wrappers make the actual Heap malloc/free path
// independently inspectable in the flashed ELF even with ordinary new linked.
extern "C" [[gnu::noinline]] std::byte* heap_rx_probe(std::size_t bytes) noexcept { return bare_heap.acquire_rx(bytes); }
extern "C" [[gnu::noinline]] wire::TxBlock heap_tx_probe(std::size_t bytes) noexcept { return bare_heap.acquire_tx(bytes); }
extern "C" [[gnu::noinline]] void heap_rx_release_probe(std::byte* p) noexcept { bare_heap.release_rx(p); }
extern "C" [[gnu::noinline]] void heap_tx_release_probe(wire::TxBlock block) noexcept { bare_heap.release_tx(block); }

namespace {
void tiny_requests(bool failing) noexcept
{
    for (const std::size_t size : {0u, 1u, 128u}) {
        auto* p = heap_rx_probe(size); auto t = heap_tx_probe(size);
        if (failing) { demand(!p && !t.memory && t.granted == 0u); }
        else {
            demand(p && reinterpret_cast<uintptr_t>(p) % BareGeometry::alignment == 0u && t.memory && t.granted == size);
            if (size != 0u && p && t.memory) { p[0] = std::byte{0xA5}; t.memory[0] = std::byte{0x5A}; }
        }
        heap_rx_release_probe(p); heap_tx_release_probe(t);
    }
}
void fixed_command(char key) noexcept
{
    active = key;
    const auto live_before = mallinfo().uordblks;
    if (key == 'N') { command(key); return; } // negative control: global nothrow new still aborts
    if (key == 'M' || key == 'P') {
        exhaust(); report("BEFORE");
        if (key == 'M') { void* p = std::malloc(64u); demand(!p); std::free(p); report("MALLOC_NULL"); }
        else { demand(create_both(cobs_pool, rtu_pool) && receive_both(cobs_pool, rtu_pool)); report("POOL_OK"); }
        release_all(); recovered(live_before);
    } else if (key == 'C') { create_failure(cobs_heap, live_before); }
    else if (key == 'R') { create_failure(rtu_heap, live_before); }
    else if (key == 'F') { create_failure(framed_heap, live_before); }
    else if (key == 'c') { rx_failure(cobs_heap, 0u, live_before); }
    else if (key == 'r') { rx_failure(rtu_heap, 1u, live_before); }
    else if (key == 'f') { rx_failure(framed_heap, 2u, live_before); }
    else if (key == 'G') { growth_failure(cobs_heap, 0u, live_before); }
    else if (key == 'g') { growth_failure(rtu_heap, 1u, live_before); }
    else if (key == 'J') { growth_failure(framed_heap, 2u, live_before); }
    else if (key == 'Z') {
        tiny_requests(false); exhaust(); report("BEFORE"); tiny_requests(true); report("REFUSED", 6u);
        release_all(); tiny_requests(false); recovered(live_before);
    } else { ++failures; report("UNKNOWN"); }
}
}
extern "C" void bench_init() { baseline_bench_init(); }
extern "C" void bench_loop()
{
    uint8_t key;
    if (HAL_UART_Receive(&huart3, &key, 1u, 1u) != HAL_OK) { return; }
    if (key == 'H') {
        emit("HELLO"); field(2u); field(SystemCoreClock); field(bench_heap_begin()); field(bench_heap_capacity());
        field(failures); field(std::get_new_handler() == nullptr ? 0u : 1u); emit("\n");
    } else { fixed_command(static_cast<char>(key)); }
}
