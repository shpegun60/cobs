/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Paired Heap/Pool and CRC-method measurements on real H7S silicon.
// Hot scopes exclude byte-oracle checks and the serial report channel.
#define UART_ENGINE_IMPLEMENT
#include "uart/Uart.h"
#include "uart_bench.h"
#include "usart.h"
#include "crc.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/cobs/UartAdapter.h"
#include "adapters/rtu/UartAdapter.h"
#include "adapters/stm32/Crc16.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

#ifndef HEAP_CRC_PROTOCOL
#define HEAP_CRC_PROTOCOL 0
#endif
#ifndef HEAP_CRC_POLICY
#define HEAP_CRC_POLICY 3
#endif

BenchCounter g_bench_usart_irq, g_bench_rx_dma_irq, g_bench_tx_dma_irq;
extern "C" uintptr_t bench_heap_begin();
extern "C" uint32_t bench_heap_capacity();
extern "C" uint32_t bench_heap_committed();

namespace {
using Bytes = std::span<const uint8_t>;
#if HEAP_CRC_POLICY == 0
using Integrity = crc::NoCrc;
#elif HEAP_CRC_POLICY == 1
using Integrity = crc::Crc16Bitwise;
#elif HEAP_CRC_POLICY == 2
using Integrity = crc::Crc16Table;
#elif HEAP_CRC_POLICY == 3
using Integrity = crc::stm32::Crc16;
#endif
Integrity integrity() noexcept
{
#if HEAP_CRC_POLICY == 3
    return Integrity{hcrc};
#else
    return {};
#endif
}
constexpr bool is_cobs = HEAP_CRC_PROTOCOL == 0;
namespace framing = modbus::rtu::framing;
struct PrivateFramer : framing::Standard<framing::Direction::Request> {
    static constexpr framing::Layout layout(framing::Direction d, uint8_t f) noexcept
    { return f == 0x41u ? framing::Layout::length_prefixed(2u) : framing::standard_layout(d, f); }
};
template<bool Heap, bool Wide>
using Link = std::conditional_t<is_cobs,
    cobs::Endpoint<std::conditional_t<Heap, wire::Heap, wire::Pool<8, 2>>,
                   cobs::Format<Integrity, Wide ? 1024u : 253u>>,
    modbus::rtu::Endpoint<std::conditional_t<Heap, wire::Heap, wire::Pool<8, 2>>,
                          modbus::rtu::Format<Integrity, (Wide ? 1028u : 254u) - Integrity::wire_size>, PrivateFramer>>;
template<bool Heap, bool Wide> Link<Heap, Wide> links{integrity()}; // AXI SRAM, not the DTCM stack
alignas(32) std::array<uint8_t, 4104u> payload;
alignas(32) std::array<uint8_t, 1050u> candidate;
uint32_t failures = 0u, core_rows = 0u, raw_rows = 0u;
[[maybe_unused]] volatile uint32_t sink = 0u;

void emit(const char* text) noexcept
{
    if (HAL_UART_Transmit(&huart3, reinterpret_cast<const uint8_t*>(text),
        static_cast<uint16_t>(std::strlen(text)), 2000u) != HAL_OK) { Error_Handler(); }
}
uint32_t tick() noexcept
{
    asm volatile("" ::: "memory");
    const uint32_t value = DWT->CYCCNT;
    asm volatile("" ::: "memory");
    return value;
}
void fill(const std::size_t size, const uint32_t pattern) noexcept
{
    uint32_t state = 0xC0B50000u ^ static_cast<uint32_t>(size);
    for (std::size_t i = 0u; i < size; ++i) {
        state ^= state << 13u; state ^= state >> 17u; state ^= state << 5u;
        payload[i] = pattern == 1u ? 0u : static_cast<uint8_t>(state);
    }
}
struct Borrow {
    Bytes bytes{};
    bool held = false;
    bool send(Bytes value) noexcept { if (held) { return false; } bytes = value; held = true; return true; }
    bool busy() const noexcept { return held; }
    void complete() noexcept { held = false; bytes = {}; }
};
template<class L> typename L::Message message(L& link, const std::size_t size) noexcept
{
    if constexpr (is_cobs) { return link.make_message(size); }
    else { return link.make_message(0x11u, 0x41u, size + 2u); }
}
template<class L> Bytes body_of(const typename L::Packet& packet) noexcept
{
    if constexpr (is_cobs) { return packet.data(); }
    else {
        const auto data = packet.data();
        if (data.size() < 2u || ((static_cast<std::size_t>(data[0]) << 8u) | data[1]) != data.size() - 2u) {
            ++failures; return {};
        }
        return data.subspan(2u);
    }
}
template<class L> bool send_body(L& link, const Bytes body, const bool growing = false) noexcept
{
    auto m = message(link, growing ? 0u : body.size());
    if (!m) { return false; }
    if (!growing) { if (!m.append_bytes(body)) { return false; } }
    else {
        for (std::size_t pos = 0u; pos < body.size(); pos += 16u) {
            if (!m.append_bytes(body.subspan(pos, std::min<std::size_t>(16u, body.size() - pos)))) { return false; }
        }
    }
    return link.send(m) == wire::SendResult::Sent;
}
struct Cycles { uint32_t rx = 0u, tx = 0u, release = 0u; };
template<class L>
bool echo(L& link, Borrow& borrow, const Bytes input, const Bytes expected, const bool growing, Cycles& c) noexcept
{
    auto start = tick();
    link.consume(input);
    auto packet = link.pop_packet();
    c.rx += tick() - start;
    if (!packet) { return false; }
    const auto body = body_of<L>(packet);
    if (!std::ranges::equal(body, expected)) { return false; }
    start = tick();
    const bool sent = send_body(link, body, growing);
    c.tx += tick() - start;
    const bool same = sent && std::ranges::equal(borrow.bytes, input);
    start = tick();
    borrow.complete(); link.poll(0u); packet.reset();
    c.release += tick() - start;
    return same && !link.tx_active() && !link.has_packet();
}
struct Fragmentation {
    std::array<void*, 96u> blocks{};
    explicit Fragmentation(const bool enabled) noexcept
    {
        if (!enabled) { return; }
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            const auto size = 24u + (i * 137u) % 1000u;
            blocks[i] = std::malloc(size);
            if (!blocks[i]) { ++failures; } else { std::memset(blocks[i], 0xD3, size); }
        }
        for (std::size_t i = 0; i < blocks.size(); i += 2u) { std::free(blocks[i]); blocks[i] = nullptr; }
    }
    ~Fragmentation() { for (void* block : blocks) { std::free(block); } }
};

template<bool Heap, bool Wide>
void core_sample(std::size_t size, uint32_t pattern, uint32_t scenario, uint32_t sample,
                 Bytes input, Bytes body) noexcept
{
    auto& link = links<Heap, Wide>;
    Borrow borrow;
    if (!link.bind(typename Link<Heap, Wide>::Sender{tiny::bind<&Borrow::send>(borrow)},
                   typename Link<Heap, Wide>::BusyQuery{tiny::bind<&Borrow::busy>(borrow)})) { ++failures; return; }
    Cycles warm;
    const bool growing = scenario == 1u;
    if (!echo(link, borrow, input, body, growing, warm)) { ++failures; return; }
    const auto live_before = mallinfo().uordblks;
    const auto iterations = Wide ? 1u : 4u;
    Cycles cycles;
    bool ok = true;
    const uint32_t mask = __get_PRIMASK(); __disable_irq();
    const auto started = tick();
    for (uint32_t i = 0u; i < iterations; ++i) { ok = echo(link, borrow, input, body, growing, cycles) && ok; }
    const auto window = tick() - started;
    __set_PRIMASK(mask);
    const auto live_after = mallinfo().uordblks;
    ok = ok && live_before == live_after && window < 600000u;
    if (!ok) { ++failures; }
    char line[240];
    (void)std::snprintf(line, sizeof(line), "R,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%d,%d,%u\n",
        Heap ? 1u : 0u, Wide ? 1u : 0u, static_cast<unsigned long>(size), static_cast<unsigned long>(pattern),
        static_cast<unsigned long>(scenario), static_cast<unsigned long>(sample), static_cast<unsigned long>(iterations),
        static_cast<unsigned long>(cycles.rx), static_cast<unsigned long>(cycles.tx), static_cast<unsigned long>(cycles.release),
        static_cast<unsigned long>(window), static_cast<unsigned long>(input.size()), live_before, live_after, ok ? 1u : 0u);
    emit(line); ++core_rows;
    (void)link.unbind(); // never retain a delegate to the local Borrow
}
template<bool Wide> void core_case(const std::size_t size, const uint32_t pattern, const uint32_t scenario) noexcept
{
    Fragmentation fragments{scenario == 2u};
    auto& link = links<false, Wide>;
    Borrow borrow;
    (void)link.bind(typename Link<false, Wide>::Sender{tiny::bind<&Borrow::send>(borrow)},
                    typename Link<false, Wide>::BusyQuery{tiny::bind<&Borrow::busy>(borrow)});
    fill(size, pattern);
    const Bytes body{payload.data(), size};
    if (!send_body(link, body) || borrow.bytes.size() > candidate.size()) { ++failures; return; }
    const auto count = borrow.bytes.size();
    std::copy(borrow.bytes.begin(), borrow.bytes.end(), candidate.begin());
    borrow.complete(); link.poll(0u); (void)link.unbind();
    const Bytes input{candidate.data(), count};
    char line[80];
    (void)std::snprintf(line, sizeof(line), "F,%u,%lu,%lu,%lu,%lu,", Wide ? 1u : 0u,
        static_cast<unsigned long>(size), static_cast<unsigned long>(pattern), static_cast<unsigned long>(scenario),
        static_cast<unsigned long>(count)); emit(line);
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : input) { const char pair[]{hex[byte >> 4u], hex[byte & 15u], '\0'}; emit(pair); }
    emit("\n");
    for (uint32_t s = 0u; s < 9u; ++s) {
        if ((s & 1u) == 0u) { core_sample<false, Wide>(size, pattern, scenario, s, input, body); }
        core_sample<true, Wide>(size, pattern, scenario, s, input, body);
        if ((s & 1u) != 0u) { core_sample<false, Wide>(size, pattern, scenario, s, input, body); }
    }
}
void core() noexcept
{
    core_rows = 0u;
    for (uint32_t mode = 0u; mode < 3u; ++mode) {
        for (uint32_t pattern = 0u; pattern < 2u; ++pattern) {
            for (const std::size_t n : {0u, 8u, 32u, 128u, 250u}) { core_case<false>(n, pattern, mode); }
            core_case<true>(1024u, pattern, mode);
        }
    }
    char line[90];
    (void)std::snprintf(line, sizeof(line), "ENDCORE,%lu,%lu,%lu\n", static_cast<unsigned long>(core_rows),
        static_cast<unsigned long>(failures), static_cast<unsigned long>(bench_heap_committed())); emit(line);
}
[[maybe_unused]] uint32_t digest_word(uint32_t hash, uint16_t value) noexcept
{
    hash = (hash ^ (value & 255u)) * 16777619u;
    return (hash ^ (value >> 8u)) * 16777619u;
}
void verify_hardware() noexcept
{
#if HEAP_CRC_POLICY == 3
    auto policy = integrity();
    fill(4104u, 0u);
    uint32_t tested = 0u, bad = 0u, digest = 2166136261u;
    for (std::size_t offset = 0u; offset < 8u; ++offset) {
        for (std::size_t size = 0u; size <= 1024u; ++size) {
            const Bytes input{payload.data() + offset, size};
            const auto actual = policy.calculate(input);
            if (actual != crc::Crc16Bitwise{}.calculate(input)) { ++bad; }
            digest = digest_word(digest, actual); ++tested;
        }
    }
    const auto empty = policy.calculate({});
    if (empty != 0xFFFFu) { ++bad; }
    failures += bad;
    char line[100];
    (void)std::snprintf(line, sizeof(line), "VERIFY,%lu,%lu,%lu,%u\n", static_cast<unsigned long>(tested),
        static_cast<unsigned long>(bad), static_cast<unsigned long>(digest), empty); emit(line);
#else
    emit("VERIFY,0,0,0,0\n");
#endif
}
void raw_crc() noexcept
{
    raw_rows = 0u;
#if HEAP_CRC_POLICY != 0
    auto policy = integrity();
    fill(4104u, 0u);
    for (const std::size_t size : {0u, 1u, 2u, 3u, 4u, 7u, 8u, 16u, 32u, 64u, 128u, 250u, 252u, 256u, 1024u, 4096u}) {
        for (const std::size_t offset : {0u, 1u}) {
            const Bytes input{payload.data() + offset, size};
            const auto expected = crc::Crc16Bitwise{}.calculate(input);
            const uint32_t iterations = size >= 1024u ? 1u : 16u;
            for (uint32_t sample = 0u; sample < 9u; ++sample) {
                sink = policy.calculate(input);
                uint32_t sum = 0u;
                const auto mask = __get_PRIMASK(); __disable_irq();
                const auto started = tick();
                for (uint32_t i = 0; i < iterations; ++i) {
                    asm volatile("" ::: "memory"); sum += policy.calculate(input);
                }
                const auto cycles = tick() - started;
                __set_PRIMASK(mask);
                sink = sum;
                const bool ok = sum == iterations * expected && cycles < 600000u;
                if (!ok) { ++failures; }
                char line[120];
                (void)std::snprintf(line, sizeof(line), "C,%lu,%lu,%lu,%lu,%lu,%u,%u\n",
                    static_cast<unsigned long>(size), static_cast<unsigned long>(offset), static_cast<unsigned long>(sample),
                    static_cast<unsigned long>(iterations), static_cast<unsigned long>(cycles), expected, ok ? 1u : 0u);
                emit(line); ++raw_rows;
            }
        }
    }
#endif
    char line[80];
    (void)std::snprintf(line, sizeof(line), "ENDCRC,%lu,%lu\n", static_cast<unsigned long>(raw_rows),
        static_cast<unsigned long>(failures)); emit(line);
}

using Serial = Uart<256, 4>;
Serial serial;
template<bool Heap> using Adapter = std::conditional_t<is_cobs,
    cobs::UartAdapter<Serial, Link<Heap, false>>, modbus::rtu::UartAdapter<Serial, Link<Heap, false>>>;
Adapter<false> pool_adapter{serial, links<false, false>};
Adapter<true> heap_adapter{serial, links<true, false>};
bool uart_mode = false, heap_mode = false;
volatile bool work = false;
bool pending_switch = false;
uint32_t echo_frames = 0u, echo_bytes = 0u, service_calls = 0u;
uint64_t service_cycles = 0u;
constexpr std::array<uint8_t, 4> magic{0xB6, 'H', 'C', 0xA5};
void wake() noexcept { work = true; }
uint64_t irq_cycles() noexcept { return g_bench_usart_irq.total + g_bench_rx_dma_irq.total + g_bench_tx_dma_irq.total; }

template<bool Heap> void serve() noexcept
{
    auto& link = links<Heap, false>;
    if constexpr (Heap) { heap_adapter.proceed(); } else { pool_adapter.proceed(); }
    if (link.tx_active()) { return; }
    if (pending_switch) {
        // First complete/reclaim the ACK's TX, then switch a single bound adapter.
        pending_switch = false;
        bool ok;
        if constexpr (Heap) { ok = heap_adapter.unbind() && pool_adapter.bind(); }
        else { ok = pool_adapter.unbind() && heap_adapter.bind(); }
        if (!ok) { ++failures; } else { heap_mode = !Heap; }
        return;
    }
    auto packet = link.pop_packet();
    if (!packet) { return; }
    const auto body = body_of<Link<Heap, false>>(packet);
    if (body.size() == 5u && std::equal(magic.begin(), magic.end(), body.begin())) {
        if (body[4] == 'M') {
            if (!send_body(link, body)) { ++failures; }
            pending_switch = true;
        } else if (body[4] == 'S') {
            // Coherent cumulative cut. Inclusive service windows subtract all
            // measured USART/DMA ISR cycles, so the CPU numerator never counts
            // those twice. SysTick/outer idle loop are not a whole-system CPU meter.
            const auto mask = __get_PRIMASK(); __disable_irq();
            const auto irq = irq_cycles();
            const auto u = serial.stats();
            const std::array<uint32_t, 18> words{2u, HEAP_CRC_PROTOCOL, HEAP_CRC_POLICY, Heap ? 1u : 0u,
                SystemCoreClock, HAL_GetTick(), echo_frames, echo_bytes, failures,
                static_cast<uint32_t>(service_cycles), static_cast<uint32_t>(service_cycles >> 32u),
                static_cast<uint32_t>(irq), static_cast<uint32_t>(irq >> 32u), service_calls,
                u.rx_overrun, u.rx_errors, u.tx_errors, u.restarts};
            __set_PRIMASK(mask);
            auto m = message(link, 5u + sizeof(words));
            if (!m || !m.append_bytes(body) || !m.append_le(std::span<const uint32_t>{words}) ||
                link.send(m) != wire::SendResult::Sent) { ++failures; }
        } else { ++failures; }
    } else {
        if (!send_body(link, body)) { ++failures; }
        ++echo_frames; echo_bytes += static_cast<uint32_t>(body.size());
    }
}
void uart_service() noexcept
{
    const auto now = HAL_GetTick();
    static uint32_t last = 0u;
    const auto remaining = heap_mode ? heap_adapter.deadline_in_ms() : pool_adapter.deadline_in_ms();
    if (!work && now - last < 50u && remaining != 0u) { __WFI(); return; }
    last = now;
    auto mask = __get_PRIMASK(); __disable_irq();
    work = false;
    const auto irq_before = irq_cycles();
    const auto started = tick();
    __set_PRIMASK(mask);
    if (heap_mode) { serve<true>(); } else { serve<false>(); }
    mask = __get_PRIMASK(); __disable_irq();
    const auto elapsed = tick() - started;
    const auto irq = irq_cycles() - irq_before;
    if (elapsed < irq) { ++failures; } else { service_cycles += elapsed - irq; }
    ++service_calls;
    __set_PRIMASK(mask);
}
void hello() noexcept
{
    char line[220];
    (void)std::snprintf(line, sizeof(line), "HELLO,2,%u,%u,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
        HEAP_CRC_PROTOCOL, HEAP_CRC_POLICY, static_cast<unsigned long>(SystemCoreClock),
        static_cast<unsigned long>(HAL_RCC_GetHCLKFreq()), static_cast<unsigned long>(SCB->CPUID),
        static_cast<unsigned long>(SCB->CCR), static_cast<unsigned long>(DWT->CTRL),
        static_cast<unsigned long>(bench_heap_begin()), static_cast<unsigned long>(bench_heap_capacity()),
        static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&links<false, false>)),
        static_cast<unsigned long>(sizeof(Link<false, false>)), static_cast<unsigned long>(sizeof(Link<true, false>)));
    emit(line);
}
} // namespace

extern "C" void bench_init()
{
    SCB_EnableICache(); SCB_EnableDCache();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#if defined(__CORTEX_M) && (__CORTEX_M == 7U)
    *reinterpret_cast<volatile uint32_t*>(DWT_BASE + 0xFB0u) = 0xC5ACCE55u;
#endif
    DWT->CYCCNT = 0u; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    huart3.Init.BaudRate = 115200u;
    if (HAL_UART_Init(&huart3) != HAL_OK) { Error_Handler(); }
}
extern "C" void bench_loop()
{
    if (uart_mode) { uart_service(); return; }
    uint8_t command;
    if (HAL_UART_Receive(&huart3, &command, 1u, 1u) != HAL_OK) { return; }
    if (command == 'H') { hello(); }
    if (command == 'V') { verify_hardware(); }
    if (command == 'C') { raw_crc(); }
    if (command == 'B') { core(); }
    if (command == 'U') {
        emit("UART,1000000\n");
        huart3.Init.BaudRate = 1000000u;
        if (HAL_UART_Init(&huart3) != HAL_OK || HAL_UARTEx_EnableFifoMode(&huart3) != HAL_OK ||
            !serial.init(&huart3) || !pool_adapter.bind()) { Error_Handler(); }
        serial.setWakeHandler(Serial::WakeHandler{wake});
        uart_mode = true;
    }
}
