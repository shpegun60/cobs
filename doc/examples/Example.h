/* Author: shpegun60; SPDX-License-Identifier: MIT */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace example {
inline unsigned checks = 0;
inline void check(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "line %d: %s failed\n", line, expression);
        std::exit(1);
    }
}

// A bounded, copying transport. No UART, socket, allocation or hidden queue.
// Set occupied/refuse to exercise backpressure and start failure explicitly.
struct Transport {
    std::array<uint8_t, 4096> bytes{};
    std::size_t size = 0;
    bool occupied = false;
    bool refuse = false;

    bool send(std::span<const uint8_t> frame) noexcept
    {
        if (occupied || refuse || frame.size() > bytes.size()) { return false; }
        std::copy(frame.begin(), frame.end(), bytes.begin());
        size = frame.size();
        return true;
    }
    bool busy() const noexcept { return occupied; }
    std::span<const uint8_t> frame() const noexcept { return {bytes.data(), size}; }
};

template<class Link>
bool bind(Link& link, Transport& transport) noexcept
{
    return link.bind(
        typename Link::Sender{tiny::bind<&Transport::send>(transport)},
        typename Link::BusyQuery{tiny::bind<&Transport::busy>(transport)});
}

inline int finish(const char* name)
{
    std::printf("%s: %u checks passed\n", name, checks);
    return 0;
}
} // namespace example

#define CHECK(...) ::example::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
