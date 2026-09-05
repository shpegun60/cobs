/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Real qmake consumer for cobs/cobs.pri.
 *
 * This is intentionally application-shaped: only Cobs.h is included. Both
 * built-in storage strategies instantiate the same use path, while the .pri
 * fragment supplies and links the non-template codec sources exactly once.
 */

#include "Cobs.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <type_traits>
#include <utility>

namespace {

class Loopback final {
public:
	[[nodiscard]] bool send(const std::span<const uint8_t> frame) noexcept
	{
		if (m_busy || frame.size() > m_bytes.size()) {
			return false;
		}
		std::copy(frame.begin(), frame.end(), m_bytes.begin());
		m_size = frame.size();
		m_busy = true;
		return true;
	}

	[[nodiscard]] bool busy() const noexcept { return m_busy; }

	[[nodiscard]] std::span<const uint8_t> frame() const noexcept
	{
		return {m_bytes.data(), m_size};
	}

	void finish() noexcept { m_busy = false; }

private:
	std::array<uint8_t, 128> m_bytes{};
	std::size_t m_size = 0;
	bool m_busy = false;
};

template<class Engine>
[[nodiscard]] bool exercise()
{
	Engine endpoint;
	Loopback loopback;
	if (!endpoint.bind(
			typename Engine::Sender{tiny::bind<&Loopback::send>(loopback)},
			typename Engine::BusyQuery{tiny::bind<&Loopback::busy>(loopback)})) {
		return false;
	}

	constexpr std::array<uint8_t, 5> payload{0x11, 0x00, 0x22, 0x33, 0x44};
	auto message = endpoint.make_message(payload.size());
	if (!message || !message.append_be(uint16_t{0x1100u}) ||
	    !message.append_le(uint16_t{0x3322u}) ||
	    !message.append_native(uint8_t{0x44u})) {
		return false;
	}
	if (endpoint.send(message) != cobs::SendResult::Sent || message) {
		return false;
	}

	endpoint.consume(loopback.frame());
	auto packet = endpoint.pop_packet();
	if (!packet || packet.size() != payload.size() ||
	    !std::equal(packet.data().begin(), packet.data().end(), payload.begin())) {
		return false;
	}
	std::size_t offset = 0u;
	uint16_t first = 0u;
	uint16_t second = 0u;
	uint8_t last = 0u;
	if (!cobs::read_be(packet.data(), offset, first) ||
	    !cobs::read_le(packet.data(), offset, second) ||
	    !cobs::read_native(packet.data(), offset, last) ||
	    first != 0x1100u || second != 0x3322u || last != 0x44u ||
	    offset != packet.size()) {
		return false;
	}

	loopback.finish();
	endpoint.poll();
	const cobs::Stats snapshot = endpoint.stats();
	return !endpoint.tx_active() && endpoint.unbind() &&
	       snapshot.rx.frames_delivered == 1 && snapshot.tx.frames_sent == 1;
}

} // namespace

int main()
{
	using Wire = cobs::Format<>;
	using HeapEndpoint = cobs::Endpoint<>;
	using PoolEndpoint = cobs::Endpoint<wire::Pool<2, 2>>;

	static_assert(wire::ByteStorage<typename HeapEndpoint::Storage>);
	static_assert(wire::ByteStorage<typename PoolEndpoint::Storage>);
	static_assert(std::is_same_v<typename HeapEndpoint::Format, Wire>);
	static_assert(HeapEndpoint::max_receive_size == 253);
	static_assert(HeapEndpoint::max_send_size == 253);
	static_assert(HeapEndpoint::length_size == 1);
	static_assert(HeapEndpoint::length_size == PoolEndpoint::length_size);

	if (!exercise<HeapEndpoint>()) {
		return 1;
	}
	if (!exercise<PoolEndpoint>()) {
		return 2;
	}

	std::puts("qmake COBS consumer passed");
	return 0;
}
