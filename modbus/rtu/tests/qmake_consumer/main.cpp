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
	link.poll();
	const auto stats = link.stats();
	return !link.tx_active() && link.unbind() &&
	       stats.rx.frames_received == 1u && stats.tx.frames_sent == 1u;
}

} // namespace

int main()
{
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
