/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Direct RTU is safe only when another layer supplies a complete ADU.
// UART IDLE/TC callbacks alone do not promise that, regardless of chunk size.
#include "modbus/rtu/Rtu.h"
#include "Example.h"

int main()
{
    example::Transport transport;
    modbus::rtu::Endpoint<> sender, burst;
    CHECK(example::bind(sender, transport));
    auto message = sender.make_message(0x11u, 0x03u);
    CHECK(message && message.append_be(uint16_t{0x006B}) && message.append_be(uint16_t{3}));
    CHECK(sender.send(message) == wire::SendResult::Sent);
    sender.poll(0u);

    // An external datagram/framing layer supplied exactly this candidate.
    burst.receive_adu(transport.frame());
    auto packet = burst.pop_packet();
    CHECK(packet && packet.size() == 4u);

    // Splitting the same candidate is not a way to stream into receive_adu().
    burst.receive_adu(transport.frame().first(3u));
    burst.receive_adu(transport.frame().subspan(3u));
    CHECK(!burst.has_packet()); // demonstration vector, not a CRC collision proof

    namespace framing = modbus::rtu::framing;
    using Framed = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
        framing::Standard<framing::Direction::Request>>;
    Framed stream;
    stream.consume(transport.frame().first(3u));
    CHECK(stream.assembling() && !stream.has_packet());
    stream.consume(transport.frame().subspan(3u));
    auto complete = stream.pop_packet();
    CHECK(complete);
    stream.consume(transport.frame().first(3u));
    // An explicit application event says the transaction was abandoned.
    // For STM32 automatic stale handling, use rtu_adapter.cpp instead.
    stream.expire_incomplete();
    CHECK(!stream.assembling() && stream.framing_stats().stale_frames == 1u);
    return example::finish("rtu_direct");
}
