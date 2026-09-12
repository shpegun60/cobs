/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Diagnostic only: do not repair/replace the allocator being diagnosed.
// Linker observers forward to the real abort/_exit after reporting the path.
#define UART_ENGINE_IMPLEMENT
#include "uart/Uart.h"
#include "uart_bench.h"
#include "usart.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include <array>
#include <cstdlib>
#include <cstring>
#include <new>

BenchCounter g_bench_usart_irq, g_bench_rx_dma_irq, g_bench_tx_dma_irq;
extern "C" uintptr_t bench_heap_begin();
extern "C" uint32_t bench_heap_capacity();
extern "C" uint32_t bench_heap_committed();
extern "C" [[noreturn]] void __real_abort();
extern "C" [[noreturn]] void __real__exit(int status);

namespace {
cobs::Endpoint<> cobs_heap;
modbus::rtu::Endpoint<> rtu_heap;
cobs::Endpoint<wire::Pool<2, 1>> cobs_pool;
modbus::rtu::Endpoint<wire::Pool<2, 1>> rtu_pool;
std::array<void*, 256u> held{};
uint32_t held_count = 0u, held_bytes = 0u, refused = 0u, failures = 0u;
char active = '-';
constexpr std::array<uint8_t, 7> cobs_frame{6u, 4u, 0x41u, 0x42u, 0xB1u, 0xD1u, 0u};
constexpr std::array<uint8_t, 8> rtu_frame{0x11u, 3u, 0u, 0u, 0u, 1u, 0x86u, 0x9Au};
constexpr std::array<uint8_t, 32> body{};

void emit(const char* text) noexcept
{
    // No printf/stdio, formatting heap, new_handler or allocation in observers.
    const auto size = static_cast<uint16_t>(std::strlen(text));
    if (HAL_UART_Transmit(&huart3, reinterpret_cast<const uint8_t*>(text), size, 1000u) != HAL_OK) { Error_Handler(); }
}
void number(uint32_t value) noexcept
{
    char digits[11];
    auto pos = sizeof(digits) - 1u;
    digits[pos] = '\0';
    do { digits[--pos] = static_cast<char>('0' + value % 10u); value /= 10u; } while (value != 0u);
    emit(digits + pos);
}
void field(uint32_t value) noexcept { emit(","); number(value); }
void report(const char* stage, uint32_t detail = 0u) noexcept
{
    emit(stage); emit(","); const char tag[]{active, '\0'}; emit(tag);
    field(held_count); field(held_bytes); field(refused); field(bench_heap_committed());
    field(failures); field(detail); emit("\n");
}
void demand(const bool condition) noexcept { if (!condition) { ++failures; } }

void exhaust() noexcept
{
    // Fill the arena with real live malloc allocations, then consume all small
    // remnants too. Failure of malloc(1) proves no ordinary message can fit.
    for (const std::size_t bytes : {4096u, 256u, 16u, 1u}) {
        while (true) {
            void* const p = std::malloc(bytes);
            if (!p) { ++refused; break; }
            if (held_count == held.size()) { std::free(p); ++failures; return; }
            held[held_count++] = p;
            held_bytes += static_cast<uint32_t>(bytes);
            std::memset(p, 0xA5, bytes);
        }
    }
    void* const tiny = std::malloc(1u);
    demand(tiny == nullptr);
    std::free(tiny);
    demand(refused == 4u && held_bytes > 120000u && held_count < held.size());
    demand(std::get_new_handler() == nullptr);
    report("EXHAUSTED");
}
void release_all() noexcept
{
    while (held_count != 0u) { std::free(held[--held_count]); held[held_count] = nullptr; }
    held_bytes = 0u;
}
template<class C, class R> bool receive_both(C& c, R& r) noexcept
{
    c.consume(cobs_frame); r.receive_adu(rtu_frame);
    auto cp = c.pop_packet(); auto rp = r.pop_packet();
    return cp && rp && cp.data().size() == 2u && cp.data()[0] == 0x41u && cp.data()[1] == 0x42u &&
        rp.data().size() == 4u && rp.data()[0] == 0u && rp.data()[1] == 0u && rp.data()[2] == 0u && rp.data()[3] == 1u;
}
template<class C, class R> bool create_both(C& c, R& r) noexcept
{
    auto cm = c.make_message(body.size()); auto rm = r.make_message(0x11u, 3u, body.size());
    return cm && rm && cm.append_bytes(body) && rm.append_bytes(body);
}
template<class M> void grow(M& message) noexcept
{
    demand(static_cast<bool>(message));
    demand(message.append_bytes(std::span{body}.first(1u)));
    demand(message.capacity() < body.size() + 1u);
    exhaust(); report("BEFORE", static_cast<uint32_t>(message.capacity()));
    const bool result = message.append_bytes(body);
    report(result ? "RETURNED_VALUE" : "RETURNED_NULL");
}
void command(const char key) noexcept
{
    active = key;
    demand(held_count == 0u && failures == 0u && std::get_new_handler() == nullptr);
    if (key == 'G') { auto m = cobs_heap.make_message(1u); grow(m); return; }
    if (key == 'g') { auto m = rtu_heap.make_message(0x11u, 3u, 1u); grow(m); return; }
    exhaust(); report("BEFORE");
    if (key == 'M') {
        void* const p = std::malloc(64u); demand(p == nullptr); std::free(p);
        report("MALLOC_NULL");
    } else if (key == 'P') {
        demand(create_both(cobs_pool, rtu_pool) && receive_both(cobs_pool, rtu_pool));
        report("POOL_OK");
    } else if (key == 'N') {
        void* const p = ::operator new(64u, std::nothrow);
        report(p ? "RETURNED_VALUE" : "RETURNED_NULL"); ::operator delete(p);
    } else if (key == 'C') {
        auto m = cobs_heap.make_message(250u); report(m ? "RETURNED_VALUE" : "RETURNED_NULL");
    } else if (key == 'R') {
        auto m = rtu_heap.make_message(0x11u, 3u, 250u); report(m ? "RETURNED_VALUE" : "RETURNED_NULL");
    } else if (key == 'c') {
        cobs_heap.consume(cobs_frame); auto p = cobs_heap.pop_packet(); report(p ? "RETURNED_VALUE" : "RETURNED_NULL");
    } else if (key == 'r') {
        rtu_heap.receive_adu(rtu_frame); auto p = rtu_heap.pop_packet(); report(p ? "RETURNED_VALUE" : "RETURNED_NULL");
    } else { ++failures; }
    release_all();
    demand(create_both(cobs_heap, rtu_heap) && receive_both(cobs_heap, rtu_heap));
    report("RECOVERED");
}
} // namespace

extern "C" [[noreturn]] void __wrap_abort()
{
    report("ABORT");
    __real_abort(); // observe, do NOT turn the failure into recovery
}
extern "C" [[noreturn]] void __wrap__exit(const int status)
{
    report("EXIT", static_cast<uint32_t>(status));
    __real__exit(status); // original Cube _exit loop; runner must reset us
}
extern "C" void bench_init()
{
    huart3.Init.BaudRate = 115200u;
    if (HAL_UART_Init(&huart3) != HAL_OK) { Error_Handler(); }
    demand(create_both(cobs_heap, rtu_heap) && receive_both(cobs_heap, rtu_heap));
}
extern "C" void bench_loop()
{
    uint8_t key;
    if (HAL_UART_Receive(&huart3, &key, 1u, 1u) != HAL_OK) { return; }
    if (key == 'H') {
        emit("HELLO"); field(1u); field(SystemCoreClock); field(bench_heap_begin()); field(bench_heap_capacity());
        field(failures); field(std::get_new_handler() == nullptr ? 0u : 1u); emit("\n");
    } else { command(static_cast<char>(key)); }
}
