/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Same regressions on the host and real MCU; no assert(), exceptions or heap
// required. Memory is supplied by the harness, never by the test body.
#ifndef WIRE_TEST_CONTRACT_CHECKS_H_
#define WIRE_TEST_CONTRACT_CHECKS_H_

#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "modbus/tcp/Tcp.h"
#include <algorithm>
#include <array>
#include <cstring>

namespace contract_checks {

struct ConstStore : crc::Crc16Bitwise {
	static void store(uint8_t* destination, const uint16_t& value) noexcept
	{ crc::Codec<uint16_t>::store(destination, value); }
	static void store(uint8_t*, uint16_t&&) = delete;
};
struct OverloadedStore : crc::Crc16Bitwise {
	inline static unsigned wrong_calls = 0u;
	static void store(uint8_t* destination, const uint16_t& value) noexcept
	{ crc::Codec<uint16_t>::store(destination, value); }
	// A real user overload could throw here. The MCU build has no exceptions;
	// record entry instead, retaining the same potentially-throwing signature.
	static void store(uint8_t* destination, uint16_t&& value) noexcept(false)
	{ ++wrong_calls; crc::Codec<uint16_t>::store(destination, value); }
};
static_assert(crc::Policy<ConstStore> && crc::Policy<OverloadedStore>);

struct BadCalculate : crc::Crc16Bitwise {
	uint16_t calculate(const std::span<const uint8_t>&) noexcept { return 0u; }
	uint16_t calculate(std::span<const uint8_t>&&) noexcept(false) { return 0u; }
};
struct BadDestination : crc::Crc16Bitwise {
	static void store(uint8_t* const&, const uint16_t&) noexcept {}
	static void store(uint8_t*&&, const uint16_t&) noexcept(false) {}
};
struct BadSource : crc::Crc16Bitwise {
	static uint16_t load(const uint8_t* const&) noexcept { return 0u; }
	static uint16_t load(const uint8_t*&&) noexcept(false) { return 0u; }
};
static_assert(!crc::Policy<BadCalculate> && !crc::Policy<BadDestination> && !crc::Policy<BadSource>);

enum Plain { Zero, One };
enum UnscopedFixed : uint16_t { FixedZero, FixedOne };
enum class Scoped : uint16_t { Zero, One };
enum class ScopedDefault { Zero, One };

template<class T>
constexpr bool any_reader =
	requires(std::span<const uint8_t> b, std::size_t& n, T& v) { wire::read_native(b, n, v); } ||
	requires(std::span<const uint8_t> b, std::size_t& n, T& v) { cobs::read_be(b, n, v); } ||
	requires(std::span<const uint8_t> b, std::size_t& n, T& v) { modbus::rtu::read_le(b, n, v); } ||
	requires(std::span<const uint8_t> b, std::size_t& n, T& v) { modbus::tcp::read_native(b, n, v); };
static_assert(!any_reader<Plain> && !any_reader<UnscopedFixed>);
static_assert(any_reader<Scoped> && any_reader<ScopedDefault> && any_reader<std::byte>);
static_assert(!any_reader<const uint16_t> && !any_reader<volatile uint16_t> && !any_reader<bool>);
static_assert(wire::Scalar<Plain> && wire::Scalar<UnscopedFixed>); // writer compatibility

template<class Check>
void readers(Check check)
{
	for (const uint16_t raw : {uint16_t{0u}, uint16_t{1u}, uint16_t{2u},
	                          uint16_t{0x7FFFu}, uint16_t{0x8000u}, uint16_t{0xFFFFu}}) {
		for (unsigned order = 0u; order < 3u; ++order) {
			std::array<uint8_t, 2u> bytes{};
			if (order == 0u) { std::memcpy(bytes.data(), &raw, sizeof(raw)); }
			else {
				bytes[order == 1u ? 0u : 1u] = static_cast<uint8_t>(raw >> 8u);
				bytes[order == 1u ? 1u : 0u] = static_cast<uint8_t>(raw);
			}
			Scoped value = Scoped::Zero;
			std::size_t offset = 0u;
			const bool ok = order == 0u ? cobs::read_native(bytes, offset, value)
				: order == 1u ? modbus::rtu::read_be(bytes, offset, value)
				: modbus::tcp::read_le(bytes, offset, value);
			check(ok && offset == 2u && static_cast<uint16_t>(value) == raw);
			const bool short_ok = order == 0u ? cobs::read_native(bytes, offset, value)
				: order == 1u ? modbus::rtu::read_be(bytes, offset, value)
				: modbus::tcp::read_le(bytes, offset, value);
			check(!short_ok && offset == 2u && static_cast<uint16_t>(value) == raw);
		}
	}
}

template<class E, class Check>
void endpoint(Check check)
{
	E link;
	std::array<uint8_t, 64u> frame{};
	std::size_t size = 0u;
	OverloadedStore::wrong_calls = 0u;
	check(link.bind([&](std::span<const uint8_t> bytes) noexcept {
		if (bytes.size() > frame.size()) { return false; }
		size = bytes.size(); std::copy(bytes.begin(), bytes.end(), frame.begin()); return true;
	}, []() noexcept { return false; }));
	auto message = [&]() {
		if constexpr (requires { link.make_message(); }) { return link.make_message(); }
		else if constexpr (requires { link.make_message(1u, 1u, 3u, 0u); }) {
			return link.make_message(1u, 1u, 3u, 0u);
		} else { return link.make_message(1u, 3u); }
	}();
	constexpr std::array<uint8_t, 4u> data{0u, 0x12u, 0xFEu, 0u};
	check(message && message.append_bytes(data));
	check(link.send(message) == wire::SendResult::Sent && !message);
	check(OverloadedStore::wrong_calls == 0u);
	const std::span<const uint8_t> received{frame.data(), size};
	if constexpr (requires { link.consume(received); }) { link.consume(received); }
	else { link.receive_adu(received); }
	auto packet = link.pop_packet();
	check(packet && std::ranges::equal(packet.data(), data));
	check(!link.has_packet() && link.stats().rx.frames_received == 1u);
	packet.reset(); link.poll(0u);
	check(!link.tx_active() && link.unbind());
}

template<class Memory, class Check>
void policies(Check check)
{
	endpoint<cobs::Endpoint<Memory, cobs::Format<ConstStore, 16u>>>(check);
	endpoint<modbus::rtu::Endpoint<Memory, modbus::rtu::Format<ConstStore, 16u>>>(check);
	endpoint<modbus::tcp::Endpoint<Memory, modbus::tcp::Format<ConstStore, 16u>>>(check);
	endpoint<cobs::Endpoint<Memory, cobs::Format<OverloadedStore, 16u>>>(check);
	endpoint<modbus::rtu::Endpoint<Memory, modbus::rtu::Format<OverloadedStore, 16u>>>(check);
	endpoint<modbus::tcp::Endpoint<Memory, modbus::tcp::Format<OverloadedStore, 16u>>>(check);
}

} // namespace contract_checks
#endif
