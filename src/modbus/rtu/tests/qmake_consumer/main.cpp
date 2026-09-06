/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

namespace {

class Loopback final {
public:
	bool send(const std::span<const uint8_t> adu) noexcept
	{
		if (m_busy) {
			return false;
		}
		m_frame.assign(adu.begin(), adu.end());
		m_busy = true;
		return true;
	}

	bool busy() const noexcept { return m_busy; }
	std::span<const uint8_t> frame() const noexcept { return m_frame; }
	void finish() noexcept { m_busy = false; }

private:
	std::vector<uint8_t> m_frame{};
	bool m_busy = false;
};

template<class Link>
bool exercise()
{
	Link link;
	Loopback loopback;
	if (!link.bind(
			typename Link::Sender{tiny::bind<&Loopback::send>(loopback)},
			typename Link::BusyQuery{tiny::bind<&Loopback::busy>(loopback)})) {
		return false;
	}

	auto message = link.make_message(0x11u, 0x03u, 5u);
	constexpr std::array<uint8_t, 1> tail{0xA5u};
	if (!message || !message.append_be(uint16_t{0x0010u}) ||
	    !message.append_be(uint16_t{2u}) || !message.append_bytes(tail)) {
		return false;
	}
	if (link.send(message) != modbus::SendResult::Sent || message) {
		return false;
	}

	link.receive_adu(loopback.frame());
	auto packet = link.pop_packet();
	if (!packet || packet.address() != 0x11u || packet.function() != 0x03u ||
	    packet.size() != 5u || packet.pdu().size() != 6u ||
	    packet.adu().size() != 9u || !::crc::verify<::crc::Crc16Bitwise>(packet.adu())) {
		return false;
	}

	std::size_t offset = 0u;
	uint16_t first = 0u;
	uint16_t second = 0u;
	uint8_t byte = 0u;
	if (!modbus::read_be(packet.data(), offset, first) ||
	    !modbus::read_be(packet.data(), offset, second) ||
	    !modbus::read_be(packet.data(), offset, byte) ||
	    first != 0x0010u || second != 2u || byte != 0xA5u) {
		return false;
	}

	loopback.finish();
	link.poll(0u);
	const auto stats = link.stats();
	return !link.tx_active() && link.unbind() &&
	       stats.rx.frames_received == 1u && stats.tx.frames_sent == 1u;
}

// A framed client/server pair over the same loopback: the fragment must carry
// Framing.h and detail/StreamReceiver.h, and the stream receiver must
// assemble a request cut in two.
template<class Memory>
bool exercise_framed()
{
	namespace framing = modbus::rtu::framing;
	using Client = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>,
		framing::Standard<framing::Direction::Response>>;
	using Server = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>,
		framing::Standard<framing::Direction::Request>>;
	Client client;
	Server server;
	Loopback client_wire;
	Loopback server_wire;
	if (!client.bind(typename Client::Sender{tiny::bind<&Loopback::send>(client_wire)},
	                 typename Client::BusyQuery{tiny::bind<&Loopback::busy>(client_wire)}) ||
	    !server.bind(typename Server::Sender{tiny::bind<&Loopback::send>(server_wire)},
	                 typename Server::BusyQuery{tiny::bind<&Loopback::busy>(server_wire)})) {
		return false;
	}

	auto request = client.make_message(0x11u, 0x03u);
	if (!request.append_be(uint16_t{0x006Bu}) || !request.append_be(uint16_t{3u}) ||
	    client.send(request) != modbus::SendResult::Sent) {
		return false;
	}
	const std::span<const uint8_t> on_wire = client_wire.frame();
	server.consume(on_wire.first(3u));
	if (server.has_packet()) {
		return false;
	}
	server.consume(on_wire.subspan(3u));
	auto received = server.pop_packet();
	if (!received || received.function() != 0x03u || received.size() != 4u) {
		return false;
	}

	auto reply = server.make_message(received.address(), received.function());
	if (!reply.append_be(uint8_t{6u}) || !reply.append_be(uint16_t{0x022Bu}) ||
	    !reply.append_be(uint16_t{0u}) || !reply.append_be(uint16_t{0x64u}) ||
	    server.send(reply) != modbus::SendResult::Sent) {
		return false;
	}
	client.consume(server_wire.frame());
	auto response = client.pop_packet();
	if (!response || response.size() != 7u || response.data()[0] != 6u) {
		return false;
	}

	auto inconsistent = server.make_message(0x11u, 0x03u);
	if (!inconsistent.append_be(uint8_t{6u}) ||
	    server.send(inconsistent) != modbus::SendResult::Invalid ||
	    server.framing_stats().tx_layout_rejected != 1u) {
		return false;
	}
	client_wire.finish();
	server_wire.finish();
	client.poll(0u);
	server.poll(0u);
	return server.stats().rx.frames_received == 1u && client.stats().rx.frames_received == 1u;
}

} // namespace

int main()
{
	if (!exercise_framed<wire::Heap>() || !exercise_framed<wire::Pool<2, 2>>()) {
		return 3;
	}
	if (!exercise<modbus::rtu::Endpoint<>>()) {
		return 1;
	}
	if (!exercise<modbus::rtu::Endpoint<wire::Pool<2, 2>>>()) {
		return 2;
	}
	if (!exercise<modbus::rtu::Endpoint<wire::Pool<2, 2>, modbus::rtu::Format<::crc::Crc16Table>>>()) {
		return 3;
	}
	std::puts("qmake Modbus RTU consumer passed");
	return 0;
}
