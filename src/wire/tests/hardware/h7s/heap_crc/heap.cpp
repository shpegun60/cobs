/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Test-only newlib backing arena. malloc/free/operator new remain the actual
// toolchain implementation. Both this arena and static Pools live in AXI SRAM.
// Cube's current _end is in DTCM, which is not UART-DMA accessible.
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace {
alignas(32) std::array<std::byte, 128u * 1024u> arena;
std::size_t used = 0u;
}
extern "C" void* _sbrk(const std::ptrdiff_t increment)
{
    const auto previous = used;
    if (increment >= 0) {
        const auto grow = static_cast<std::size_t>(increment);
        if (grow > arena.size() - used) { errno = ENOMEM; return reinterpret_cast<void*>(-1); }
        used += grow;
    } else {
        const auto shrink = static_cast<std::size_t>(-(increment + 1)) + 1u;
        if (shrink > used) { errno = ENOMEM; return reinterpret_cast<void*>(-1); }
        used -= shrink;
    }
    return arena.data() + previous;
}
extern "C" uintptr_t bench_heap_begin() { return reinterpret_cast<uintptr_t>(arena.data()); }
extern "C" uint32_t bench_heap_capacity() { return static_cast<uint32_t>(arena.size()); }
extern "C" uint32_t bench_heap_committed() { return static_cast<uint32_t>(used); }
