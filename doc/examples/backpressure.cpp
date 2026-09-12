/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "modbus/tcp/Tcp.h"
#include "Example.h"

template<class Link, class Make>
void retry(Make make)
{
    example::Transport transport;
    Link link;
    CHECK(example::bind(link, transport));
    auto pending = make(link); // keep this owner in the application, across calls
    CHECK(pending && pending.append_be(uint16_t{0x1234}));

    transport.occupied = true;
    CHECK(link.send(pending) == wire::SendResult::Busy);
    CHECK(pending && pending.size() == 2u);
    transport.occupied = false;

    transport.refuse = true;
    CHECK(link.send(pending) == wire::SendResult::Failed);
    CHECK(pending && !pending.append_be(uint16_t{0x5678})); // finalized, immutable
    transport.refuse = false;
    CHECK(link.send(pending) == wire::SendResult::Sent); // byte-identical retry
    CHECK(!pending && link.tx_active());
    transport.occupied = true;
    link.poll(0u);
    CHECK(link.tx_active() && !link.unbind());
    transport.occupied = false;
    link.poll(1u);
    CHECK(!link.tx_active() && link.unbind());

    // With one TX slot a second Message fails predictably; after reset it works.
    auto held = make(link);
    auto refused = make(link);
    CHECK(held && !refused);
    held = {}; // Message has move assignment, not Packet::reset()
    auto recovered = make(link);
    CHECK(recovered);
    CHECK(link.send(recovered) == wire::SendResult::Unbound);
    CHECK(recovered);
}

int main()
{
    retry<cobs::Endpoint<wire::Pool<2, 1>>>(
        [](auto& e) { return e.make_message(); });
    retry<modbus::rtu::Endpoint<wire::Pool<2, 1>>>(
        [](auto& e) { return e.make_message(1u, 0x41u); });
    retry<modbus::tcp::Endpoint<wire::Pool<2, 1>>>(
        [](auto& e) { return e.make_message(8u, 1u, 0x41u); });
    return example::finish("backpressure");
}
