/* Author: shpegun60
 * SPDX-License-Identifier: MIT
 * Paired, transport-independent endpoint timing on real H7S silicon.
 * UART is only the untimed control/results channel. No production code changes.
 */
#include "main.h"
#include "usart.h"
#include "uart_bench.h"
#define UART_ENGINE_PROBE 1
#include "uart/uart_probe.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <type_traits>

#ifndef PROTOCOL_BENCH_CRC
#define PROTOCOL_BENCH_CRC 1
#endif
#if PROTOCOL_BENCH_CRC == 0
using Integrity = crc::NoCrc;
#elif PROTOCOL_BENCH_CRC == 1
using Integrity = crc::Crc16Bitwise;
#elif PROTOCOL_BENCH_CRC == 2
using Integrity = crc::Crc16Table;
#else
#error Unsupported policy
#endif

BenchCounter g_bench_usart_irq;
BenchCounter g_bench_rx_dma_irq;
BenchCounter g_bench_tx_dma_irq;

namespace {
using Bytes = std::span<const uint8_t>;
using Memory = wire::Pool<8, 2>;
uint32_t failures = 0u;
uint32_t groups = 0u;
std::array<uint8_t, 1024> payload{};
std::array<uint8_t, 1040> candidate{};

void emit(const char* text) noexcept
{
    if (HAL_UART_Transmit(&huart3, reinterpret_cast<const uint8_t*>(text),
            static_cast<uint16_t>(std::strlen(text)), 2000u) != HAL_OK) {
        Error_Handler();
    }
}

void makePayload(const std::size_t size, const uint32_t pattern) noexcept
{
    uint32_t state = 0xC0B50000u ^ static_cast<uint32_t>(size);
    for (std::size_t i = 0; i < size; ++i) {
        state ^= state << 13u;
        state ^= state >> 17u;
        state ^= state << 5u;
        uint8_t value = static_cast<uint8_t>(state);
        if (pattern == 1u) { value = 0u; }
        if (pattern == 2u) { value = static_cast<uint8_t>(1u + i % 255u); }
        if (pattern == 3u) { value = i % 2u == 0u ? 0u : 0xA5u; }
        if (pattern == 4u) {
            value = i % 254u == 0u || i % 254u == 253u ? 0u
                : static_cast<uint8_t>(i * 17u + 3u);
        }
        payload[i] = value;
    }
}

struct Borrow {
    Bytes last{};
    bool held = false;
    bool send(const Bytes bytes) noexcept
    {
        if (held) { return false; }
        last = bytes;
        held = true;
        return true;
    }
    bool busy() const noexcept { return held; }
    void complete() noexcept { last = {}; held = false; }
};

struct Cycles {
    uint32_t rx = 0u;
    uint32_t tx = 0u;
    uint32_t release = 0u;
};

inline uint32_t tick() noexcept
{
    asm volatile("" ::: "memory");
    const uint32_t result = DWT->CYCCNT;
    asm volatile("" ::: "memory");
    return result;
}

template<bool IsCobs, class Link>
bool sendBody(Link& link, const Bytes body) noexcept
{
    auto message = [&]() noexcept {
        if constexpr (IsCobs) { return link.make_message(body.size()); }
        else { return link.make_message(0x11u, 0x41u, body.size()); }
    }();
    if (!message || !message.append_bytes(body)) { return false; }
    using Result = decltype(link.send(message));
    return link.send(message) == Result::Sent;
}

template<bool IsCobs, class Link>
bool echo(Link& link, Borrow& borrow, const Bytes input, const Bytes body,
          const std::size_t chunk, Cycles& cycles) noexcept
{
    typename Link::Packet packet;
    const uint32_t rxStart = tick();
    if constexpr (IsCobs) {
        const std::size_t step = chunk == 0u ? input.size() : chunk;
        for (std::size_t offset = 0u; offset < input.size(); offset += step) {
            link.consume(input.subspan(offset, std::min(step, input.size() - offset)));
        }
    } else {
        link.receive_adu(input);
    }
    packet = link.pop_packet();
    cycles.rx += tick() - rxStart;
    // Oracle checks are OUTSIDE the three measured scopes.
    if (!packet || packet.data().size() != body.size() ||
            std::memcmp(packet.data().data(), body.data(), body.size()) != 0) {
        return false;
    }
    if constexpr (!IsCobs) {
        if (packet.address() != 0x11u || packet.function() != 0x41u) { return false; }
    }
    const uint32_t txStart = tick();
    const bool sent = sendBody<IsCobs>(link, packet.data());
    cycles.tx += tick() - txStart;
    const bool same = sent && borrow.last.size() == input.size() &&
        std::memcmp(borrow.last.data(), input.data(), input.size()) == 0;
    const uint32_t releaseStart = tick();
    borrow.complete();
    link.poll();
    packet = {};
    cycles.release += tick() - releaseStart;
    return same && !link.tx_active() && !link.has_packet() &&
        link.storage().rx_stats().in_use == 0u && link.storage().tx_stats().in_use == 0u;
}

template<bool IsCobs, bool Wide>
void runCase(const std::size_t size, const uint32_t pattern, const std::size_t chunk) noexcept
{
    using CobsFormat = std::conditional_t<Wide, cobs::Format<Integrity, 1024>, cobs::Format<Integrity>>;
    using RtuFormat = modbus::rtu::Format<Integrity, Wide ? 1026u + Integrity::wire_size : 256u>;
    using Link = std::conditional_t<IsCobs,
        cobs::Endpoint<Memory, CobsFormat>, modbus::rtu::Endpoint<Memory, RtuFormat>>;
    Link link;
    Borrow borrow;
    if (!link.bind(typename Link::Sender{tiny::bind<&Borrow::send>(borrow)},
                   typename Link::BusyQuery{tiny::bind<&Borrow::busy>(borrow)})) {
        ++failures; return;
    }
    makePayload(size, pattern);
    const Bytes body{payload.data(), size};
    if (!sendBody<IsCobs>(link, body) || borrow.last.size() > candidate.size()) {
        borrow.complete(); link.poll(); ++failures; return;
    }
    const std::size_t wireSize = borrow.last.size();
    std::memcpy(candidate.data(), borrow.last.data(), wireSize);
    borrow.complete();
    link.poll();
    const Bytes input{candidate.data(), wireSize};
    char line[160];
    (void)std::snprintf(line, sizeof(line), "G,%u,%u,%lu,%lu,%lu,%lu,%lu,",
        IsCobs ? 0u : 1u, PROTOCOL_BENCH_CRC,
        static_cast<unsigned long>(Link::max_send_size), static_cast<unsigned long>(size),
        static_cast<unsigned long>(pattern), static_cast<unsigned long>(chunk),
        static_cast<unsigned long>(wireSize));
    emit(line);
    constexpr char hex[] = "0123456789abcdef";
    for (std::size_t i = 0u; i < wireSize;) {
        std::size_t used = 0u;
        while (i < wireSize && used + 2u < sizeof(line)) {
            line[used++] = hex[input[i] >> 4u];
            line[used++] = hex[input[i++] & 15u];
        }
        line[used] = '\0';
        emit(line);
    }
    emit("\n");
    ++groups;
    const uint32_t iterations = Wide ? 1u : 4u;
    for (uint32_t sample = 0u; sample < 9u; ++sample) {
        Cycles warm;
        if (!echo<IsCobs>(link, borrow, input, body, chunk, warm)) { ++failures; return; }
        Cycles cycles;
        bool ok = true;
        const uint32_t primask = __get_PRIMASK();
        __disable_irq();
        const uint32_t windowStart = tick();
        for (uint32_t i = 0u; i < iterations; ++i) {
            ok = echo<IsCobs>(link, borrow, input, body, chunk, cycles) && ok;
        }
        const uint32_t window = tick() - windowStart;
        __set_PRIMASK(primask);
        if (!ok || window >= 600000u) { ++failures; }
        (void)std::snprintf(line, sizeof(line), "R,%lu,%lu,%lu,%lu,%lu,%lu,%u\n",
            static_cast<unsigned long>(sample), static_cast<unsigned long>(iterations),
            static_cast<unsigned long>(cycles.rx), static_cast<unsigned long>(cycles.tx),
            static_cast<unsigned long>(cycles.release), static_cast<unsigned long>(window), ok ? 1u : 0u);
        emit(line);
    }
}

void runAll() noexcept
{
    failures = 0u;
    groups = 0u;
    char line[160];
    (void)std::snprintf(line, sizeof(line), "BEGIN,1,%u,%lu,%lu,%lu,%lu\n", PROTOCOL_BENCH_CRC,
        static_cast<unsigned long>(SystemCoreClock), static_cast<unsigned long>(SCB->CPUID),
        static_cast<unsigned long>(SCB->CCR), static_cast<unsigned long>(DWT->CTRL));
    emit(line);
    for (const std::size_t size : {8u, 32u, 128u, 252u}) {
        for (uint32_t pattern = 0u; pattern < 5u; ++pattern) {
            // Alternate ordering so one protocol is not always measured first.
            if (pattern % 2u == 0u) { runCase<false, false>(size, pattern, 0u); }
            runCase<true, false>(size, pattern, 0u);
            runCase<true, false>(size, pattern, 128u);
            if (pattern % 2u != 0u) { runCase<false, false>(size, pattern, 0u); }
        }
    }
    for (uint32_t pattern = 0u; pattern < 5u; ++pattern) {
        runCase<false, true>(1024u, pattern, 0u);
        runCase<true, true>(1024u, pattern, 0u);
        runCase<true, true>(1024u, pattern, 128u);
    }
    (void)std::snprintf(line, sizeof(line), "END,%lu,%lu\n",
        static_cast<unsigned long>(groups), static_cast<unsigned long>(failures));
    emit(line);
}
} // namespace

extern "C" void bench_init(void)
{
    SCB_EnableICache();
    SCB_EnableDCache();
    uart_probe::init();
    huart3.Init.BaudRate = 115200u;
    if (HAL_UART_Init(&huart3) != HAL_OK) { Error_Handler(); }
}

extern "C" void bench_loop(void)
{
    uint8_t command = 0u;
    if (HAL_UART_Receive(&huart3, &command, 1u, 1u) == HAL_OK && command == 'B') {
        runAll();
    }
}
