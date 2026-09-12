/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "core_cases.h"
#include <cstdio>
#include <cstdlib>

namespace {
unsigned checks = 0u;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { std::fprintf(stderr, "data_limits:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); } } while (false)

template<class Memory, class Crc, std::size_t MaxData>
void check_limit()
{
	using F = modbus::tcp::Format<Crc, MaxData>;
	using E = modbus::tcp::Endpoint<Memory, F>;
	static_assert(F::max_data_size == MaxData);
	static_assert(F::max_pdu_size == MaxData + 1u);
	static_assert(F::max_adu_size == MaxData + 8u + Crc::wire_size);
	static_assert(E::max_send_size == MaxData && E::max_receive_size == MaxData);
	static_assert(E::Geometry::tx_block_bytes == F::max_adu_size);
	static_assert(E::Geometry::rx_block_bytes % E::Geometry::alignment == 0u);
	using Largest = modbus::tcp::Format<Crc, 65533u - Crc::wire_size>;
	static_assert(Largest::max_adu_size == 65541u);
	static_assert(modbus::tcp::Format<Crc>::max_data_size == 252u);
	E endpoint;
	tcp_test::Capture capture;
	CHECK(tcp_test::bind(endpoint, capture));
	std::array<uint8_t, MaxData> data{};
	for (std::size_t i = 0u; i < data.size(); ++i) { data[i] = static_cast<uint8_t>(i * 13u); }
	auto message = endpoint.make_message(0x1234u, 0xFEu, 0x67u, 0u);
	CHECK(message && message.size() == 0u);
	CHECK(message.reserve(MaxData) && message.capacity() == MaxData);
	CHECK(message.append_bytes(data) && message.size() == MaxData);
	CHECK(!message.append_native(uint8_t{0u}));
	CHECK(!message.reserve(MaxData + 1u) && message.size() == MaxData);
	std::array<uint8_t, MaxData + 8u + Crc::wire_size> expected{};
	CHECK(tcp_test::frame<Crc>(expected.data(), 0x1234u, 0xFEu, 0x67u, data) == expected.size());
	CHECK(endpoint.send(message) == wire::SendResult::Sent);
	CHECK(capture.size == expected.size() && std::equal(expected.begin(), expected.end(), capture.bytes.begin()));
	CHECK(((static_cast<std::size_t>(capture.bytes[4]) << 8u) | capture.bytes[5]) == MaxData + 2u + Crc::wire_size);
	endpoint.consume(capture.view());
	auto packet = endpoint.pop_packet();
	CHECK(packet && packet.size() == MaxData && packet.pdu().size() == MaxData + 1u);
	CHECK(packet.adu().size() == expected.size() && std::equal(packet.data().begin(), packet.data().end(), data.begin()));
	std::array<uint8_t, 6u> oversized{};
	tcp_test::be16(oversized.data() + 4u, static_cast<uint16_t>(MaxData + 3u + Crc::wire_size));
	endpoint.consume(oversized);
	CHECK(endpoint.rx_failed() && endpoint.stats().rx.oversize == 1u && !endpoint.has_packet());
	endpoint.reset_rx();
	CHECK(packet.size() == MaxData && !endpoint.rx_failed());
	capture.active = false;
	endpoint.poll(0u);
	CHECK(!endpoint.tx_active());
}

template<class Crc>
void policies()
{
	check_limit<wire::Heap, Crc, 0u>();
	check_limit<wire::Heap, Crc, 1u>();
	check_limit<wire::Heap, Crc, 7u>();
	check_limit<wire::Heap, Crc, 252u>();
	check_limit<wire::Heap, Crc, 255u>();
	check_limit<wire::Heap, Crc, 1024u>();
	check_limit<wire::Pool<2, 2>, Crc, 0u>();
	check_limit<wire::Pool<2, 2>, Crc, 1u>();
	check_limit<wire::Pool<2, 2>, Crc, 7u>();
	check_limit<wire::Pool<2, 2>, Crc, 252u>();
	check_limit<wire::Pool<2, 2>, Crc, 255u>();
	check_limit<wire::Pool<2, 2>, Crc, 1024u>();
}
} // namespace

int main()
{
	policies<crc::NoCrc>();
	policies<crc::Crc8Bitwise>(); policies<crc::Crc8Table>();
	policies<crc::Crc16Bitwise>(); policies<crc::Crc16Table>();
	policies<crc::Crc32Bitwise>(); policies<crc::Crc32Table>();
	policies<crc::Crc64Bitwise>(); policies<crc::Crc64Table>();
	std::printf("TCP data limits: %u checks, 108 Memory/CRC/payload-limit combinations\n", checks);
}
