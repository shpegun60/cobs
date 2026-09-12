/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "modbus/tcp/Tcp.h"
#include <array>
#include <algorithm>
#include <cstdio>
template<class Memory> bool run()
{
	modbus::tcp::Endpoint<Memory> endpoint;
	constexpr std::array<uint8_t, 12u> expected{0x12, 0x34, 0, 0, 0, 6, 0x11, 3, 0, 0, 0, 10};
	bool correct = false;
	if (!endpoint.bind([&correct, &expected](std::span<const uint8_t> bytes) noexcept {
		correct = bytes.size() == expected.size() && std::equal(bytes.begin(), bytes.end(), expected.begin());
		return true;
	}, []() noexcept { return false; })) { return false; }
	auto message = endpoint.make_message(0x1234u, 0x11u, 3u);
	if (!message || !message.append_be(uint16_t{0u}) || !message.append_be(uint16_t{10u}) || endpoint.send(message) != wire::SendResult::Sent) { return false; }
	endpoint.poll(0u);
	endpoint.consume(expected);
	auto packet = endpoint.pop_packet();
	return correct && packet && packet.size() == 4u && packet.transaction_id() == 0x1234u;
}
int main()
{
	if (!run<wire::Heap>() || !run<wire::Pool<2, 2>>()) { return 1; }
	std::puts("TCP qmake consumer: Heap/Pool wire oracle passed");
}
