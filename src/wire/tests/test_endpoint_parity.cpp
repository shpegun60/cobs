/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

// One application lifecycle for COBS, burst RTU and both standard RTU roles.
// Only the message metadata and receive boundary differ. No assert(): these
// checks also execute under -DNDEBUG in the release build.
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace {
unsigned checks = 0u, failures = 0u;
void check(bool ok, const char* label)
{
	++checks;
	if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}

using Result = wire::SendResult;
constexpr std::array<uint8_t, 4> payload{0x12u, 0x34u, 0x78u, 0x56u};
struct Transport {
	std::vector<uint8_t> frame;
	bool occupied = false, accept = false;
	unsigned starts = 0u;
	bool send(std::span<const uint8_t> bytes) noexcept
	{
		++starts;
		frame.assign(bytes.begin(), bytes.end());
		occupied = accept;
		return accept;
	}
	bool busy() const noexcept { return occupied; }
};

template<class E> auto make(E& e, std::size_t hint = 0u)
{
	if constexpr (requires { e.make_message(hint); }) { return e.make_message(hint); }
	else { return e.make_message(0x11u, 0x06u, hint); } // four data bytes in either role
}

template<class E> void receive(E& e, std::span<const uint8_t> bytes)
{
	if constexpr (requires { e.consume(bytes); }) {
		// Every byte is a separate delivery; empty deliveries change nothing.
		for (const auto& byte : bytes) {
			e.consume({});
			e.consume(std::span<const uint8_t>{&byte, 1u});
		}
	} else { e.receive_adu(bytes); } // never disguise a burst endpoint as a stream
}

template<class E> void exercise(const char* label)
{
	std::printf("Endpoint parity: %s\n", label);
	Transport transport;
	E endpoint, other;
	typename E::Message empty;
	check(endpoint.send(empty) == Result::Invalid, "empty message is Invalid");
	check(!make(endpoint, E::max_send_size + 1u), "oversized factory hint refused");
	auto message = make(endpoint);
	check(message && message.size() == 0u, "fresh non-owned-prefix message is empty");
	check(message.append_be(uint16_t{0x1234u}), "append BE scalar");
	const auto old_size = message.size(), old_capacity = message.capacity();
	check(!message.reserve(E::max_send_size + 1u) && message.size() == old_size &&
	      message.capacity() == old_capacity, "failed reserve preserves message");
	check(message.append_le(uint16_t{0x5678u}), "append LE scalar");
	check(endpoint.send(message) == Result::Unbound && message.size() == payload.size(),
	      "valid unbound message stays owned");
	check(message.reserve(message.capacity()), "unbound message remains writable");
	check(other.send(message) == Result::Invalid, "foreign owner refused even with same storage type");
	check(endpoint.bind(typename E::Sender{tiny::bind<&Transport::send>(transport)},
	                    typename E::BusyQuery{tiny::bind<&Transport::busy>(transport)}), "bind pair");
	transport.occupied = true;
	check(endpoint.send(message) == Result::Busy && transport.starts == 0u,
	      "busy does not start transport");
	check(message.append_bytes({}), "busy building message stays writable");
	transport.occupied = false;
	check(endpoint.send(message) == Result::Failed && message && !endpoint.tx_active(),
	      "failed start retains prepared message and no borrow");
	const auto failed_wire = transport.frame;
	check(!message.append_native(uint8_t{0u}) && !message.reserve(message.capacity()),
	      "failed prepared message is retryable but read-only");
	transport.occupied = true;
	check(endpoint.send(message) == Result::Busy && transport.starts == 1u,
	      "busy retry does not call sender");
	transport.occupied = false;
	transport.accept = true;
	check(endpoint.send(message) == Result::Sent && !message && endpoint.tx_active(),
	      "accepted retry transfers ownership and empties message");
	check(transport.frame == failed_wire, "retry emits identical wire bytes");
	check(!endpoint.unbind(), "active borrow prevents unbind");
	{
		auto pending = make(endpoint);
		check(pending.append_bytes(payload) && endpoint.send(pending) == Result::Busy,
		      "a second valid message stays pending while TX active");
	}
	receive(endpoint, transport.frame);
	auto packet = endpoint.pop_packet();
	check(packet && packet.size() == payload.size() && std::ranges::equal(packet.data(), payload),
	      "same payload through public receive and Packet APIs");
	std::size_t offset = 0u;
	uint16_t first = 0u, second = 0u;
	check(wire::read_be(packet.data(), offset, first) && wire::read_le(packet.data(), offset, second) &&
	      first == 0x1234u && second == 0x5678u, "one payload parser serves all protocols");
	const auto end = offset;
	check(!wire::read_native(packet.data(), offset, first) && offset == end && first == 0x1234u,
	      "failed reader preserves cursor and output");
	auto held = packet;
	packet.reset();
	check(!packet && packet.data().empty() && held && std::ranges::equal(held.data(), payload),
	      "Packet copy retains data after original reset");
	transport.occupied = false;
	check(endpoint.tx_active() && !endpoint.unbind(), "idle transport still needs poll to release block");
	endpoint.poll(123u);
	check(!endpoint.tx_active() && endpoint.unbind(), "poll returns TX block then unbind succeeds");
	auto snapshot = endpoint.stats();
	check(snapshot.rx.frames_received == 1u && snapshot.tx.frames_sent == 1u &&
	      snapshot.tx.send_failed == 1u && snapshot.tx.send_refused_busy == 3u, "shared counters have same meaning");
	snapshot.rx.frames_received = 99u;
	check(endpoint.stats().rx.frames_received == 1u, "stats is a detached snapshot");

	// These test endpoints have two RX slots. held keeps one alive, ready owns
	// the second; an allocation failure must not corrupt either of them.
	receive(endpoint, failed_wire);
	check(endpoint.has_packet(), "second RX block queued");
	receive(endpoint, failed_wire);
	check(endpoint.stats().rx.allocation_failure == 1u && endpoint.stats().rx.frames_received == 2u,
	      "RX exhaustion refuses only the new frame");
	endpoint.notify_gap();
	check(endpoint.has_packet() && std::ranges::equal(held.data(), payload),
	      "gap preserves queued and application-held packets");
	held.reset();
	if constexpr (requires { E::length_size; }) {
		constexpr std::array<uint8_t, 1> delimiter{0u};
		endpoint.consume(delimiter); // COBS recovery boundary, not an RTU timing rule
	}
	receive(endpoint, failed_wire);
	check(endpoint.stats().rx.frames_received == 3u, "released RX slot is reusable after recovery");
	unsigned drained = 0u;
	while (auto ready = endpoint.pop_packet()) {
		check(std::ranges::equal(ready.data(), payload), "queued contents survive exhaustion and gap");
		++drained;
	}
	check(drained == 2u, "exactly the two successful queued frames survive");
}

namespace framing = modbus::rtu::framing;
struct PrivateFramer : framing::Standard<framing::Direction::Request> {
	static constexpr framing::Layout layout(framing::Direction d, uint8_t fn) noexcept
	{
		return fn == 0x41u ? framing::Layout::length_prefixed(2u) : framing::standard_layout(d, fn);
	}
};

void owned_prefix_contract()
{
	using E = modbus::rtu::Endpoint<wire::Pool<2u, 2u>, modbus::rtu::Format<>, PrivateFramer>;
	Transport transport;
	E endpoint;
	check(endpoint.bind(E::Sender{tiny::bind<&Transport::send>(transport)},
	                    E::BusyQuery{tiny::bind<&Transport::busy>(transport)}), "private endpoint bind");
	auto message = endpoint.make_message(0x11u, 0x41u, 0u);
	check(message.size() == 2u && message.capacity() >= 2u, "owned prefix is counted before appending");
	check(message.append_bytes(payload) && message.size() == 6u, "size counts prefix plus application body");
	transport.accept = true;
	check(endpoint.send(message) == Result::Sent, "owned-prefix frame sends");
	receive(endpoint, transport.frame);
	auto packet = endpoint.pop_packet();
	std::size_t offset = 0u;
	uint16_t length = 0u;
	std::span<const uint8_t> body;
	check(packet && packet.size() == 6u && modbus::rtu::read_be(packet.data(), offset, length) &&
	      length == payload.size() && modbus::rtu::read_bytes(packet.data(), offset, length, body) &&
	      std::ranges::equal(body, payload), "RTU data retains owned length; application explicitly reads past it");
	check(packet.address() == 0x11u && packet.function() == 0x41u &&
	      packet.pdu().size() == 7u && packet.adu().size() == 10u, "RTU metadata/envelopes are unchanged");
	transport.occupied = false;
	endpoint.poll(0u);
}
} // namespace

int main()
{
	using Memory = wire::Pool<2u, 2u>;
	exercise<cobs::Endpoint<Memory>>("COBS/CRC16");
	exercise<cobs::Endpoint<Memory, cobs::Format<crc::Crc16Table>>>("COBS/Table");
	exercise<cobs::Endpoint<Memory, cobs::Format<crc::NoCrc>>>("COBS/NoCrc");
	exercise<modbus::rtu::Endpoint<Memory>>("RTU/burst");
	exercise<modbus::rtu::Endpoint<Memory, modbus::rtu::Format<crc::NoCrc>>>("RTU/burst/NoCrc");
	exercise<modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>,
		framing::Standard<framing::Direction::Request>>>("RTU/requests");
	exercise<modbus::rtu::Endpoint<Memory, modbus::rtu::Format<crc::Crc16Table>,
		framing::Standard<framing::Direction::Response>>>("RTU/responses/Table");
	owned_prefix_contract();
	static_assert(cobs::Format<crc::Crc16Bitwise, 512u>::max_send_size == 512u);
	static_assert(modbus::rtu::Format<crc::Crc16Bitwise, 512u>::max_data_size == 508u);
	std::printf("Endpoint parity: %u checks, %u failures\n", checks, failures);
	return failures == 0u ? 0 : 1;
}
