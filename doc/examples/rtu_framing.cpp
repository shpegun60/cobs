/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "modbus/rtu/Rtu.h"
#include "Example.h"

namespace framing = modbus::rtu::framing;

// example-begin: private-framer
struct PrivateRequests : framing::Standard<framing::Direction::Request> {
    using Base = framing::Standard<framing::Direction::Request>;
    static constexpr framing::Layout layout(framing::Direction direction,
                                             uint8_t function) noexcept
    {
        if (function == 0x41u) { return framing::Layout::length_prefixed(2u); }
        return Base::layout(direction, function);
    }
};
// example-end: private-framer

int main()
{
    // example-begin: rtu-roles
    using Server = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
        framing::Standard<framing::Direction::Request>>;
    using Client = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
        framing::Standard<framing::Direction::Response>>;
    // Direction describes RX. TX validates the opposite direction's layout.
    example::Transport upstream, downstream;
    Client client;
    Server server;
    CHECK(example::bind(client, upstream) && example::bind(server, downstream));
    auto request = client.make_message(0x11u, 0x03u);
    CHECK(request && request.append_be(uint16_t{0}) && request.append_be(uint16_t{2}));
    CHECK(client.send(request) == wire::SendResult::Sent);
    server.consume(upstream.frame().first(3u));
    CHECK(!server.has_packet() && server.assembling());
    server.consume(upstream.frame().subspan(3u));
    auto packet = server.pop_packet();
    CHECK(packet && packet.address() == 0x11u && packet.function() == 3u);
    auto reply = server.make_message(packet.address(), packet.function());
    CHECK(reply && reply.append_be(uint8_t{4})); // two registers = four bytes
    CHECK(reply.append_be(uint16_t{100}) && reply.append_be(uint16_t{200}));
    CHECK(server.send(reply) == wire::SendResult::Sent);
    client.consume(downstream.frame());
    auto response = client.pop_packet();
    CHECK(response && response.data().size() == 5u);
    client.poll(0u);
    server.poll(0u);
    // example-end: rtu-roles

    // Exception response has function | 0x80 and one application exception byte.
    auto exception = server.make_message(0x11u, 0x83u);
    CHECK(exception && exception.append_be(uint8_t{2})); // illegal data address
    CHECK(server.send(exception) == wire::SendResult::Sent);
    client.consume(downstream.frame());
    auto error = client.pop_packet();
    CHECK(error && error.function() == 0x83u && error.data()[0] == 2u);
    server.poll(0u);

    // example-begin: private-prefix
    using PrivateServer = modbus::rtu::Endpoint<wire::Pool<4, 2>,
        modbus::rtu::Format<crc::Crc16Bitwise, 64>, PrivateRequests>;
    PrivateServer private_link;
    CHECK(example::bind(private_link, downstream));
    auto private_message = private_link.make_message(1u, 0x41u);
    CHECK(private_message && private_message.size() == 2u); // reserved BE16 length
    CHECK(private_message.append_be(uint32_t{0x01020304}));
    CHECK(private_link.send(private_message) == wire::SendResult::Sent);
    private_link.consume(downstream.frame());
    private_link.poll(0u);
    auto private_packet = private_link.pop_packet();
    std::size_t offset = 0;
    uint16_t length = 0;
    uint32_t value = 0;
    CHECK(private_packet && wire::read_be(private_packet.data(), offset, length));
    CHECK(wire::read_be(private_packet.data(), offset, value));
    CHECK(length == 4u && value == 0x01020304u);
    // example-end: private-prefix
    auto unsupported = private_link.make_message(1u, 0x42u);
    CHECK(unsupported);
    CHECK(private_link.send(unsupported) == wire::SendResult::Sent);
    private_link.consume(downstream.frame());
    private_link.poll(0u);
    CHECK(!private_link.has_packet());
    // Unknown TX layouts pass through; RX cannot assemble an unknown function.
    return example::finish("rtu_framing");
}
