/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "modbus/tcp/Tcp.h"
#include "Example.h"

// Shared readers, writers and ownership; only creation and RX framing differ.
template<class Link, class Make, class Receive>
void round_trip(Make make, Receive receive)
{
    example::Transport transport; // outlives the bound endpoint
    Link link;
    CHECK(example::bind(link, transport));

    // example-begin: write-fields
    auto message = make(link);
    CHECK(message && message.size() == 0u); // hint is capacity, not size
    CHECK(message.append_be(uint16_t{0x1234}));
    CHECK(message.append_le(uint32_t{0x10203040}));
    const std::array<uint8_t, 3> bytes{0u, 1u, 2u};
    CHECK(message.append_bytes(bytes));
    CHECK(link.send(message) == wire::SendResult::Sent);
    CHECK(!message); // ownership transferred, not an acknowledgement
    // example-end: write-fields

    receive(link, transport.frame());
    link.poll(0u); // copying transport is idle; release the accepted TX block
    CHECK(!link.tx_active());
    auto packet = link.pop_packet();
    CHECK(packet && packet.size() == 9u);

    // example-begin: read-fields
    std::size_t offset = 0;
    uint16_t first = 0;
    uint32_t second = 0;
    std::span<const uint8_t> tail;
    CHECK(wire::read_be(packet.data(), offset, first));
    CHECK(wire::read_le(packet.data(), offset, second));
    CHECK(wire::read_bytes(packet.data(), offset, 3u, tail));
    CHECK(first == 0x1234u && second == 0x10203040u && std::ranges::equal(tail, bytes));
    CHECK(!wire::read_be(packet.data(), offset, first));
    CHECK(offset == 9u && first == 0x1234u); // failed read changes neither

    auto retained = packet; // same immutable allocation, not another copy of data
    packet.reset();
    CHECK(retained && retained.size() == 9u);
    retained.reset(); // final reference returns the RX block
    // example-end: read-fields
    CHECK(link.unbind());
}

int main()
{
    // example-begin: protocol-selection
    using Cobs = cobs::Endpoint<wire::Pool<4, 2>>;
    using Rtu = modbus::rtu::Endpoint<wire::Pool<4, 2>>;
    using Tcp = modbus::tcp::Endpoint<wire::Pool<4, 2>>;

    round_trip<Cobs>([](auto& link) { return link.make_message(9u); },
        [](auto& link, auto frame) { link.consume(frame); });
    round_trip<Rtu>([](auto& link) { return link.make_message(0x11u, 0x41u, 9u); },
        [](auto& link, auto frame) { link.receive_adu(frame); }); // whole ADU!
    round_trip<Tcp>([](auto& link) { return link.make_message(7u, 0x11u, 0x41u, 9u); },
        [](auto& link, auto frame) { link.consume(frame); });
    // example-end: protocol-selection

    // Native representation is an explicit application choice, not a change
    // to the library-owned Length, MBAP or CRC byte order.
    example::Transport transport;
    cobs::Endpoint<> link; // same code with Heap
    CHECK(example::bind(link, transport));
    auto message = link.make_message();
    const std::array<uint16_t, 2> values{12u, 34u};
    CHECK(message.append_native(std::span<const uint16_t>{values}));
    CHECK(link.send(message) == wire::SendResult::Sent);
    link.consume(transport.frame());
    link.poll(0u);
    auto packet = link.pop_packet();
    std::array<uint16_t, 2> output{};
    std::size_t offset = 0;
    CHECK(packet);
    for (auto& value : output) { CHECK(wire::read_native(packet.data(), offset, value)); }
    CHECK(output == values);
    return example::finish("protocols");
}
