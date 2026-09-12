/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Identical exhaustive 16-bit declaration checks on hosts and live Cortex-M7.
#ifndef WIRE_TEST_LENGTH_CHECKS_H_
#define WIRE_TEST_LENGTH_CHECKS_H_

#include "modbus/tcp/Tcp.h"
#include "modbus/rtu/Rtu.h"
#include <array>

namespace length_checks {
template<class Check> struct Requests { Check check; std::size_t calls = 0, last = 0; };
template<class Check> struct RejectMemory {
    template<class G> struct For {
        Requests<Check>& requests;
        explicit For(Requests<Check>& r) noexcept : requests(r) {}
        std::byte* acquire_rx(std::size_t n) noexcept {
            requests.check(n <= G::rx_block_bytes);
            ++requests.calls; requests.last = n; return nullptr;
        }
        void release_rx(std::byte* p) noexcept { requests.check(p == nullptr); }
        wire::TxBlock acquire_tx(std::size_t) noexcept { return {}; }
        void release_tx(wire::TxBlock b) noexcept { requests.check(b.memory == nullptr); }
    };
};
// Real backing storage for every skipped span; bounded flash even on the MCU.
inline constexpr std::array<uint8_t, 1024> skipped{};
template<class Endpoint>
void skip_body(Endpoint& endpoint, std::size_t bytes) {
    while (bytes > skipped.size()) {
        endpoint.consume(skipped);
        bytes -= skipped.size();
    }
    endpoint.consume(std::span<const uint8_t>{skipped}.first(bytes));
}

template<class Crc, std::size_t MaxData, class Check>
void tcp_lengths(Check check) {
    using E = modbus::tcp::Endpoint<RejectMemory<Check>, modbus::tcp::Format<Crc, MaxData>>;
    Requests<Check> calls{check};
    E endpoint{std::in_place, calls};
    unsigned accepted = 0;
    for (unsigned length = 0; length <= 65535; ++length) {
        endpoint.reset_rx();
        const auto before = calls.calls;
        const uint8_t h[]{0xFF, 0xFF, 0, 0, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
        const auto header = std::span<const uint8_t>{h};
        const auto cut = length % 7;
        endpoint.consume(header.first(cut));
        endpoint.consume(header.subspan(cut));
        const bool valid = length >= 2 + Crc::wire_size && length <= 2 + Crc::wire_size + MaxData;
        check(endpoint.rx_failed() == !valid);
        check(calls.calls == before + (valid ? 1u : 0u));
        check(endpoint.assembling() == valid);
        if (valid) {
            ++accepted;
            check(calls.last == sizeof(modbus::tcp::detail::RxBlock<typename E::Storage>) + 6 + length);
            endpoint.consume(std::span<const uint8_t>{skipped}.first(1));
            skip_body(endpoint, length - 1);
            check(!endpoint.assembling() && !endpoint.rx_failed() && !endpoint.has_packet());
        }
    }
    check(endpoint.stats().rx.candidates == 65536);
    check(endpoint.stats().rx.skipped_frames == accepted && accepted == MaxData + 1);
    for (unsigned pid = 1; pid <= 65535; ++pid) {
        endpoint.reset_rx();
        const auto before = calls.calls;
        const uint8_t h[]{0, 0, static_cast<uint8_t>(pid >> 8), static_cast<uint8_t>(pid),
            0, static_cast<uint8_t>(2 + Crc::wire_size)};
        endpoint.consume(h);
        check(endpoint.rx_failed() && calls.calls == before);
    }
    check(endpoint.stats().rx.invalid_protocol == 65535);
}

namespace f = modbus::rtu::framing;
struct Counted : f::Standard<f::Direction::Request> {
    static f::Layout layout(f::Direction, uint8_t) noexcept { return f::Layout::length_prefixed(2); }
};
template<class Crc, std::size_t MaxData, class Check>
void rtu_lengths(Check check) {
    using E = modbus::rtu::Endpoint<RejectMemory<Check>, modbus::rtu::Format<Crc, MaxData>, Counted>;
    Requests<Check> calls{check};
    E endpoint{std::in_place, calls};
    unsigned accepted = 0;
    for (unsigned count = 0; count <= 65535; ++count) {
        endpoint.discard_incomplete();
        const auto before = calls.calls;
        const uint8_t prefix[]{1, 65, static_cast<uint8_t>(count >> 8), static_cast<uint8_t>(count)};
        endpoint.consume(prefix);
        const bool valid = 2u + count <= MaxData;
        check(calls.calls == before + (valid ? 1u : 0u));
        if (valid) {
            ++accepted;
            check(calls.last == sizeof(modbus::rtu::RxBlock<typename E::Storage>) + 4 + count + Crc::wire_size);
            skip_body(endpoint, count + Crc::wire_size);
        }
        check(!endpoint.assembling() && !endpoint.has_packet());
    }
    check(endpoint.stats().rx.candidates == 65536);
    check(endpoint.stats().rx.allocation_failure == accepted);
    check(endpoint.stats().rx.oversize == 65536 - accepted);
}

template<class Check>
void run(Check check) {
    tcp_lengths<crc::NoCrc, 0>(check); tcp_lengths<crc::NoCrc, 252>(check); tcp_lengths<crc::NoCrc, 65533>(check);
    tcp_lengths<crc::Crc8Table, 0>(check); tcp_lengths<crc::Crc16Bitwise, 252>(check); tcp_lengths<crc::Crc64Table, 1024>(check);
    rtu_lengths<crc::NoCrc, 0>(check); rtu_lengths<crc::NoCrc, 65533>(check);
    rtu_lengths<crc::Crc16Bitwise, 252>(check); rtu_lengths<crc::Crc64Table, 65525>(check);
}
} // namespace length_checks
#endif
