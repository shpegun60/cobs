/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "modbus/tcp/Tcp.h"
#include "Example.h"

// example-begin: custom-memory
struct Counts { unsigned rx = 0, tx = 0; };
struct ObservedMemory {
    template<class Geometry>
    class For {
        // The implementation can instead be an arena, slabs or external RAM.
        // Neither the geometry nor these four operations know the protocol.
        wire::Pool<4, 2>::For<Geometry> pool_;
        Counts& counts_;
    public:
        explicit For(Counts& counts) noexcept : counts_(counts) {}
        std::byte* acquire_rx(std::size_t bytes) noexcept {
            ++counts_.rx;
            return pool_.acquire_rx(bytes);
        }
        void release_rx(std::byte* memory) noexcept { pool_.release_rx(memory); }
        wire::TxBlock acquire_tx(std::size_t bytes) noexcept {
            ++counts_.tx;
            return pool_.acquire_tx(bytes);
        }
        void release_tx(wire::TxBlock block) noexcept { pool_.release_tx(block); }
    };
};
// example-end: custom-memory

// example-begin: adaptive-crc
struct AdaptiveCrc : crc::Codec<uint16_t, 2, std::endian::little> {
    uint16_t calculate(std::span<const uint8_t> bytes) noexcept {
        return bytes.size() < 32u ? crc::Crc16Bitwise{}.calculate(bytes)
                                  : crc::Crc16Table{}.calculate(bytes);
    }
};
// example-end: adaptive-crc

// A stateful peripheral-shaped policy, deliberately computing a private sum.
// This is not hardware, CRC-16/MODBUS, or an error-detection recommendation.
struct SumState { unsigned calls = 0; };
struct StatefulSum : crc::Codec<uint32_t, 4, std::endian::little> {
    SumState& state;
    explicit StatefulSum(SumState& instance) noexcept : state(instance) {}
    uint32_t calculate(std::span<const uint8_t> bytes) noexcept {
        ++state.calls;
        uint32_t value = 0;
        for (uint8_t byte : bytes) { value += byte; }
        return value;
    }
};
static_assert(crc::Policy<AdaptiveCrc> && crc::Policy<StatefulSum>);

template<class Link, class Make, class Receive>
void custom(Make make, Receive receive)
{
    Counts counts;
    SumState state;
    example::Transport transport;
    // example-begin: inject-state
    Link link{StatefulSum{state}, std::in_place, counts};
    CHECK(example::bind(link, transport));
    auto message = make(link);
    CHECK(message && message.append_be(uint16_t{0x0102}));
    CHECK(link.send(message) == wire::SendResult::Sent);
    receive(link, transport.frame());
    link.poll(0u);
    auto packet = link.pop_packet();
    CHECK(packet && packet.size() == 2u);
    CHECK(state.calls == 2u); // the same policy instance handled TX and RX
    CHECK(counts.rx == 1u && counts.tx == 1u);
    // example-end: inject-state
}

int main()
{
    using Cobs = cobs::Endpoint<ObservedMemory, cobs::Format<StatefulSum, 64>>;
    using Rtu = modbus::rtu::Endpoint<ObservedMemory, modbus::rtu::Format<StatefulSum, 64>>;
    using Tcp = modbus::tcp::Endpoint<ObservedMemory, modbus::tcp::Format<StatefulSum, 64>>;
    custom<Cobs>([](auto& e) { return e.make_message(); },
                 [](auto& e, auto bytes) { e.consume(bytes); });
    custom<Rtu>([](auto& e) { return e.make_message(1u, 0x41u); },
                [](auto& e, auto bytes) { e.receive_adu(bytes); });
    custom<Tcp>([](auto& e) { return e.make_message(1u, 1u, 0x41u); },
                [](auto& e, auto bytes) { e.consume(bytes); });

    using Slow = cobs::Endpoint<wire::Pool<4, 2>, cobs::Format<crc::Crc16Bitwise>>;
    using Fast = cobs::Endpoint<wire::Pool<4, 2>, cobs::Format<crc::Crc16Table>>;
    static_assert(std::same_as<Slow::Storage, Fast::Storage>);
    static_assert(std::same_as<Slow::Message, Fast::Message>);
    static_assert(std::same_as<Slow::Packet, Fast::Packet>);
    static_assert(Slow::Geometry::rx_block_bytes % Slow::Geometry::alignment == 0);

    using Plain = cobs::Endpoint<wire::Pool<4, 2>, cobs::Format<crc::NoCrc, 255>>;
    static_assert(Plain::max_send_size == 255u);
    const std::array<uint8_t, 3> small{1, 2, 3};
    const std::array<uint8_t, 64> large{};
    CHECK(AdaptiveCrc{}.calculate(small) == crc::Crc16Bitwise{}.calculate(small));
    CHECK(AdaptiveCrc{}.calculate(large) == crc::Crc16Bitwise{}.calculate(large));
    return example::finish("policies");
}
