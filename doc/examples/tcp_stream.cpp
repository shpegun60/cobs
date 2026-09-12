/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "modbus/tcp/Tcp.h"
#include "Example.h"

int main()
{
    example::Transport transport;
    modbus::tcp::Endpoint<> sender;
    using Link = modbus::tcp::Endpoint<wire::Pool<1, 1>>;
    Link receiver;
    CHECK(example::bind(sender, transport));
    auto message = sender.make_message(0x1234u, 0x11u, 0x03u);
    CHECK(message && message.append_be(uint16_t{0}) && message.append_be(uint16_t{10}));
    CHECK(sender.send(message) == wire::SendResult::Sent);
    sender.poll(0u);
    const std::array<uint8_t, 12> expected{0x12, 0x34, 0, 0, 0, 6, 0x11, 3, 0, 0, 0, 10};
    CHECK(std::ranges::equal(transport.frame(), expected));

    // example-begin: mbap-stream
    for (uint8_t byte : expected) { receiver.consume({&byte, 1u}); }
    auto packet = receiver.pop_packet();
    CHECK(packet && packet.transaction_id() == 0x1234u);
    CHECK(packet.unit_id() == 0x11u && packet.function() == 3u);
    CHECK(packet.data().size() == 4u && packet.pdu().size() == 5u);
    CHECK(std::ranges::equal(packet.adu(), expected));

    // Keep the only RX slot: OOM skips exactly one known frame, without losing sync.
    receiver.consume(expected);
    CHECK(!receiver.has_packet() && !receiver.rx_failed());
    CHECK(receiver.stats().rx.allocation_failure == 1u);
    CHECK(receiver.stats().rx.skipped_frames == 1u);
    packet.reset();
    receiver.consume(expected);
    packet = receiver.pop_packet();
    CHECK(packet);
    packet.reset();

    auto invalid = expected;
    invalid[3] = 1u; // Protocol ID must be zero
    receiver.consume(invalid);
    CHECK(receiver.rx_failed());
    receiver.consume(expected);
    CHECK(!receiver.has_packet()); // no speculative header search

    // A real adapter closes the failed connection first. This test now supplies
    // a NEW stream, starting with a whole MBAP header, not the old stream's tail.
    receiver.reset_rx();
    receiver.consume(expected);
    packet = receiver.pop_packet();
    CHECK(packet && !receiver.rx_failed());
    // example-end: mbap-stream
    return example::finish("tcp_stream");
}
