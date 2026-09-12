/* Author: shpegun60; SPDX-License-Identifier: MIT */
#ifndef MODBUS_TCP_TEST_CORE_CASES_H_
#define MODBUS_TCP_TEST_CORE_CASES_H_
#include "modbus/tcp/Tcp.h"
#include <algorithm>
#include <array>
#include <limits>

// The same allocation-free test body runs on host and Cortex-M7.
namespace tcp_test {
struct Result { unsigned checks = 0u; unsigned failed_line = 0u; };
#define TCP_REQUIRE(...) do { ++result.checks; if (!(__VA_ARGS__)) { result.failed_line = __LINE__; return result; } } while (false)

inline void be16(uint8_t* p, uint16_t value) noexcept
{
	p[0] = static_cast<uint8_t>(value >> 8u);
	p[1] = static_cast<uint8_t>(value);
}

// Independent bit-loop oracle for the built-in default models. Never calls
// production calculate/store/load or scalar codecs to construct expected ADUs.
template<class Crc>
void trailer(std::span<const uint8_t> body, uint8_t* destination) noexcept
{
	constexpr auto width = Crc::wire_size;
	if constexpr (width != 0u) {
		constexpr bool reflected = width == 2u || width == 4u;
		constexpr uint64_t mask = width == 8u ? UINT64_MAX : (UINT64_MAX >> ((8u - width) * 8u));
		constexpr uint64_t polynomial = width == 1u ? 7u : width == 2u ? 0xA001u :
			width == 4u ? UINT64_C(0xEDB88320) : UINT64_C(0x42F0E1EBA9EA3693);
		uint64_t value = reflected ? mask : 0u;
		for (const auto byte : body) {
			value ^= reflected ? byte : static_cast<uint64_t>(byte) << ((width - 1u) * 8u);
			for (unsigned bit = 0u; bit < 8u; ++bit) {
				if constexpr (reflected) { value = (value & 1u) != 0u ? (value >> 1u) ^ polynomial : value >> 1u; }
				else { value = ((value & (UINT64_C(1) << (width * 8u - 1u))) != 0u ? (value << 1u) ^ polynomial : value << 1u) & mask; }
			}
		}
		if constexpr (width == 4u) { value ^= mask; }
		for (std::size_t i = 0u; i < width; ++i) {
			destination[i] = static_cast<uint8_t>(value >> ((reflected ? i : width - 1u - i) * 8u));
		}
	} else { (void)body; (void)destination; }
}

template<class Crc>
std::size_t frame(uint8_t* out, uint16_t transaction, uint8_t unit, uint8_t function,
	std::span<const uint8_t> data) noexcept
{
	const std::size_t total = 8u + data.size() + Crc::wire_size;
	be16(out, transaction);
	out[2] = 0u; out[3] = 0u;
	be16(out + 4u, static_cast<uint16_t>(total - 6u));
	out[6] = unit; out[7] = function;
	if (!data.empty()) { std::memcpy(out + 8u, data.data(), data.size()); }
	trailer<Crc>({out, total - Crc::wire_size}, out + total - Crc::wire_size);
	return total;
}

struct Capture {
	std::array<uint8_t, 1100u> bytes{};
	std::size_t size = 0u;
	const uint8_t* borrowed = nullptr;
	bool active = false;
	bool accept = true;
	bool send(std::span<const uint8_t> input) noexcept
	{
		if (input.size() > bytes.size()) { return false; }
		size = input.size();
		std::memcpy(bytes.data(), input.data(), size);
		if (!accept) { return false; }
		borrowed = input.data();
		active = true;
		return true;
	}
	bool busy() const noexcept { return active; }
	std::span<const uint8_t> view() const noexcept { return {bytes.data(), size}; }
};

template<class Endpoint>
bool bind(Endpoint& endpoint, Capture& transport) noexcept
{
	return endpoint.bind(
		typename Endpoint::Sender{tiny::bind<&Capture::send>(transport)},
		typename Endpoint::BusyQuery{tiny::bind<&Capture::busy>(transport)});
}

template<class Memory, class Crc>
Result core_cases() noexcept
{
	Result result;
	using E = modbus::tcp::Endpoint<Memory, modbus::tcp::Format<Crc>>;
	E endpoint;
	Capture transport;
	TCP_REQUIRE(bind(endpoint, transport));
	TCP_REQUIRE(!endpoint.bind({}, {}) && !endpoint.has_packet() && !endpoint.assembling());
	std::array<uint8_t, E::max_frame_size> expected{};
	std::array<uint8_t, E::max_send_size> data{};
	for (std::size_t i = 0u; i < data.size(); ++i) { data[i] = static_cast<uint8_t>(i * 37u); }
	for (std::size_t length = 0u; length <= data.size(); ++length) {
		auto message = endpoint.make_message(0x1234u, 0xFEu, 0x67u, 0u);
		TCP_REQUIRE(message && message.size() == 0u);
		TCP_REQUIRE(message.transaction_id() == 0x1234u && message.unit_id() == 0xFEu && message.function() == 0x67u);
		TCP_REQUIRE(message.append_bytes({data.data(), length}));
		TCP_REQUIRE(message.size() == length && message.capacity() <= E::max_send_size);
		TCP_REQUIRE(!message.reserve(std::numeric_limits<std::size_t>::max()));
		const auto total = frame<Crc>(expected.data(), 0x1234u, 0xFEu, 0x67u, {data.data(), length});
		TCP_REQUIRE(endpoint.send(message) == wire::SendResult::Sent && !message);
		TCP_REQUIRE(transport.size == total && std::equal(expected.begin(), expected.begin() + static_cast<std::ptrdiff_t>(total), transport.bytes.begin()));
		TCP_REQUIRE(endpoint.tx_active() && !endpoint.unbind());
		endpoint.poll(0u);
		TCP_REQUIRE(endpoint.tx_active() && std::memcmp(transport.borrowed, expected.data(), total) == 0);
		endpoint.consume({});
		for (std::size_t i = 0u; i < total; ++i) { endpoint.consume({expected.data() + i, 1u}); }
		TCP_REQUIRE(!endpoint.assembling() && !endpoint.rx_failed());
		auto packet = endpoint.pop_packet();
		TCP_REQUIRE(packet && packet.size() == length && packet.pdu().size() == length + 1u && packet.adu().size() == total);
		TCP_REQUIRE(packet.transaction_id() == 0x1234u && packet.unit_id() == 0xFEu && packet.function() == 0x67u);
		TCP_REQUIRE(std::equal(packet.data().begin(), packet.data().end(), data.begin()));
		auto copy = packet;
		packet = copy; // same block assignment
		packet.reset();
		TCP_REQUIRE(copy && copy.adu().data() != transport.borrowed);
		typename E::Packet moved = std::move(copy);
		TCP_REQUIRE(moved && !copy);
		transport.active = false;
		endpoint.poll(UINT32_MAX);
		TCP_REQUIRE(!endpoint.tx_active() && moved.transaction_id() == 0x1234u);
	}
	TCP_REQUIRE(!endpoint.make_message(1u, 1u, 1u, E::max_send_size + 1u));
	TCP_REQUIRE(!endpoint.make_message(1u, 1u, 1u, std::numeric_limits<std::size_t>::max()));

	const auto total = frame<Crc>(expected.data(), 0xFFFFu, 0u, 0x80u, {data.data(), 4u});
	for (std::size_t split = 0u; split <= total; ++split) {
		endpoint.consume({expected.data(), split});
		endpoint.consume({expected.data() + split, total - split});
		auto packet = endpoint.pop_packet();
		TCP_REQUIRE(packet && packet.transaction_id() == 0xFFFFu && packet.function() == 0x80u);
	}
	std::array<uint8_t, 2u * E::max_frame_size> train{};
	std::memcpy(train.data(), expected.data(), total);
	std::memcpy(train.data() + total, expected.data(), total);
	endpoint.consume({train.data(), total * 2u});
	auto held = endpoint.pop_packet();
	TCP_REQUIRE(held && endpoint.has_packet());
	endpoint.consume({expected.data(), 7u});
	TCP_REQUIRE(endpoint.assembling());
	endpoint.notify_gap();
	TCP_REQUIRE(endpoint.rx_failed() && !endpoint.assembling() && endpoint.has_packet());
	endpoint.consume({train.data(), total * 2u});
	TCP_REQUIRE(endpoint.pop_packet() && !endpoint.has_packet());
	endpoint.reset_rx();
	TCP_REQUIRE(!endpoint.rx_failed() && held.size() == 4u);
	held.reset();

	const std::array<std::array<uint8_t, 6u>, 4u> bad{{
		{{0u, 1u, 0u, 1u, 0u, 2u}}, {{0u, 1u, 0u, 0u, 0u, 0u}},
		{{0u, 1u, 0u, 0u, 0u, 1u}}, {{0u, 1u, 0u, 0u, 0xFFu, 0xFFu}}
	}};
	for (const auto& prefix : bad) {
		endpoint.consume(prefix);
		TCP_REQUIRE(endpoint.rx_failed() && !endpoint.has_packet());
		endpoint.consume({expected.data(), total});
		TCP_REQUIRE(!endpoint.has_packet());
		endpoint.reset_rx();
		endpoint.consume({expected.data(), total});
		TCP_REQUIRE(endpoint.pop_packet());
	}
	TCP_REQUIRE(endpoint.stats().rx.invalid_protocol == 1u && endpoint.stats().rx.invalid_length == 2u && endpoint.stats().rx.oversize == 1u);
	if constexpr (Crc::wire_size != 0u) {
		for (std::size_t byte = 6u; byte < total; ++byte) {
			for (unsigned bit = 0u; bit < 8u; ++bit) {
				expected[byte] ^= static_cast<uint8_t>(1u << bit);
				endpoint.consume({expected.data(), total});
				TCP_REQUIRE(endpoint.rx_failed() && !endpoint.has_packet());
				expected[byte] ^= static_cast<uint8_t>(1u << bit);
				endpoint.reset_rx();
			}
		}
		TCP_REQUIRE(endpoint.stats().rx.crc_errors == (total - 6u) * 8u);
	}

	// Failed send finalizes once; retry stays byte-identical and non-mutable.
	auto message = endpoint.make_message(7u, 8u, 9u);
	TCP_REQUIRE(message.append_be(uint16_t{0x1234u}) && message.append_le(uint16_t{0x5678u}));
	transport.accept = false;
	TCP_REQUIRE(endpoint.send(message) == wire::SendResult::Failed && message);
	const auto saved = transport.bytes;
	TCP_REQUIRE(!message.append_native(uint8_t{0u}) && !message.reserve(0u));
	transport.accept = true;
	TCP_REQUIRE(endpoint.send(message) == wire::SendResult::Sent && saved == transport.bytes);
	auto pending = endpoint.make_message(1u, 1u, 1u);
	TCP_REQUIRE(pending && endpoint.send(pending) == wire::SendResult::Busy);
	endpoint.reset_rx(); // must NOT release an active transport borrow
	TCP_REQUIRE(endpoint.tx_active());
	transport.active = false;
	endpoint.poll(0u);
	TCP_REQUIRE(endpoint.unbind() && endpoint.send(pending) == wire::SendResult::Unbound);
	TCP_REQUIRE(pending.append_bytes({}));
	typename E::Message empty;
	TCP_REQUIRE(endpoint.send(empty) == wire::SendResult::Invalid);
	typename E::Packet no_packet;
	TCP_REQUIRE(!no_packet && no_packet.data().empty() && no_packet.pdu().empty() && no_packet.adu().empty());
	TCP_REQUIRE(no_packet.transaction_id() == 0u && no_packet.unit_id() == 0u && no_packet.function() == 0u);
	return result;
}
#undef TCP_REQUIRE
} // namespace tcp_test
#endif
