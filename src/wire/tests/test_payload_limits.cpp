/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "modbus/tcp/Tcp.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0u;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { std::fprintf(stderr, "payload_limits:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); } } while (false)
template<class E>
auto make(E& endpoint, std::size_t hint)
{
	if constexpr (requires { endpoint.make_message(hint); }) { return endpoint.make_message(hint); }
	else if constexpr (requires { endpoint.make_message(0x1234u, 1u, 3u, hint); }) { return endpoint.make_message(0x1234u, 1u, 3u, hint); }
	else { return endpoint.make_message(1u, 3u, hint); }
}
template<class E, std::size_t N>
void exercise()
{
	static_assert(E::max_send_size == N && E::max_receive_size == N);
	E endpoint;
	std::array<uint8_t, 1100u> wire{};
	std::size_t count = 0u;
	CHECK(endpoint.bind([&wire, &count](std::span<const uint8_t> bytes) noexcept {
		if (bytes.size() > wire.size()) { return false; }
		count = bytes.size(); std::copy(bytes.begin(), bytes.end(), wire.begin()); return true;
	}, []() noexcept { return false; }));
	std::array<uint8_t, N> data{};
	for (std::size_t i = 0u; i < N; ++i) { data[i] = static_cast<uint8_t>(i * 29u); }
	auto message = make(endpoint, 0u);
	CHECK(message && message.size() == 0u);
	CHECK(message.reserve(N) && message.capacity() == N);
	CHECK(message.append_bytes(data) && message.size() == N);
	CHECK(!message.append_native(uint8_t{0u}) && !message.reserve(N + 1u));
	CHECK(endpoint.send(message) == wire::SendResult::Sent && !message);
	const std::span<const uint8_t> bytes{wire.data(), count};
	if constexpr (requires { endpoint.consume(bytes); }) { endpoint.consume(bytes); }
	else { endpoint.receive_adu(bytes); }
	auto packet = endpoint.pop_packet();
	CHECK(packet && packet.size() == N && std::equal(packet.data().begin(), packet.data().end(), data.begin()));
	CHECK(!endpoint.has_packet());
	endpoint.poll(0u);
	CHECK(!endpoint.tx_active());
}

template<class Crc, std::size_t N>
void all_protocols()
{
	static_assert(modbus::rtu::Format<Crc, N>::max_adu_size == N + 2u + Crc::wire_size);
	static_assert(modbus::tcp::Format<Crc, N>::max_adu_size == N + 8u + Crc::wire_size);
	exercise<cobs::Endpoint<wire::Heap, cobs::Format<Crc, N>>, N>();
	exercise<modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<Crc, N>>, N>();
	exercise<modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<Crc, N>>, N>();
	using Pool = wire::Pool<2, 2>;
	exercise<cobs::Endpoint<Pool, cobs::Format<Crc, N>>, N>();
	exercise<modbus::rtu::Endpoint<Pool, modbus::rtu::Format<Crc, N>>, N>();
	exercise<modbus::tcp::Endpoint<Pool, modbus::tcp::Format<Crc, N>>, N>();
}
template<class Crc>
void limits()
{
	all_protocols<Crc, 0u>(); all_protocols<Crc, 1u>(); all_protocols<Crc, 7u>();
	all_protocols<Crc, 252u>(); all_protocols<Crc, 255u>(); all_protocols<Crc, 1024u>();
}
} // namespace
int main()
{
	static_assert(modbus::rtu::Endpoint<>::max_send_size == 252u);
	static_assert(modbus::rtu::Endpoint<>::max_frame_size == 256u);
	static_assert(std::same_as<modbus::rtu::Endpoint<>::Crc, crc::Crc16Bitwise>);
	static_assert(modbus::tcp::Endpoint<>::max_send_size == 252u);
	static_assert(modbus::tcp::Endpoint<>::max_frame_size == 260u);
	static_assert(std::same_as<modbus::tcp::Endpoint<>::Crc, crc::NoCrc>);
	static_assert(cobs::Endpoint<>::max_send_size == 253u);
	limits<crc::NoCrc>(); limits<crc::Crc8Bitwise>(); limits<crc::Crc8Table>();
	limits<crc::Crc16Bitwise>(); limits<crc::Crc16Table>();
	limits<crc::Crc32Bitwise>(); limits<crc::Crc32Table>();
	limits<crc::Crc64Bitwise>(); limits<crc::Crc64Table>();
	std::printf("Shared payload limits: %u checks, 324 protocol/Memory/CRC/limit combinations\n", checks);
}
