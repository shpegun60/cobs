/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "core_cases.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
unsigned checks = 0u;
#define CHECK(...) do { ++checks; if (!(__VA_ARGS__)) { std::fprintf(stderr, "advanced:%d: %s\n", __LINE__, #__VA_ARGS__); std::abort(); } } while (false)

struct State {
	struct Record { std::byte* memory = nullptr; std::size_t granted = 0u; bool tx = false; };
	std::array<Record, 32u> records{};
	unsigned rx_calls = 0u, tx_calls = 0u, releases = 0u;
	bool fail_rx = false, fail_tx = false, short_tx = false;
	std::size_t overgrant = 0u;
	std::byte* allocate(std::size_t granted, bool tx) noexcept
	{
		for (auto& record : records) {
			if (record.memory != nullptr) { continue; }
			auto* memory = static_cast<std::byte*>(std::malloc(granted + 16u));
			CHECK(memory != nullptr);
			std::memset(memory + granted, 0xA5, 16u);
			record = {memory, granted, tx};
			return memory;
		}
		return nullptr;
	}
	void release(std::byte* memory, std::size_t granted, bool tx) noexcept
	{
		if (memory == nullptr) { return; }
		for (auto& record : records) {
			if (record.memory != memory) { continue; }
			CHECK(record.tx == tx && (!tx || record.granted == granted));
			for (std::size_t i = 0u; i < 16u; ++i) { CHECK(memory[record.granted + i] == std::byte{0xA5}); }
			std::free(memory);
			record = {};
			++releases;
			return;
		}
		CHECK(false); // double release or wrong storage instance
	}
	std::size_t live() const noexcept
	{
		std::size_t n = 0u;
		for (const auto& record : records) { if (record.memory != nullptr) { ++n; } }
		return n;
	}
};
struct Memory {
	template<class G> struct For {
		using Geometry = G;
		State* state;
		For() = delete;
		explicit For(State& value) noexcept : state(&value) {}
		std::byte* acquire_rx(std::size_t bytes) noexcept
		{
			++state->rx_calls;
			CHECK(bytes <= G::rx_block_bytes);
			return state->fail_rx ? nullptr : state->allocate(bytes, false);
		}
		void release_rx(std::byte* memory) noexcept { state->release(memory, 0u, false); }
		wire::TxBlock acquire_tx(std::size_t bytes) noexcept
		{
			++state->tx_calls;
			CHECK(bytes <= G::tx_block_bytes);
			if (state->fail_tx) { return {}; }
			const auto granted = state->short_tx ? bytes - 1u : bytes + state->overgrant;
			return {state->allocate(granted, true), granted};
		}
		void release_tx(wire::TxBlock block) noexcept { state->release(block.memory, block.granted, true); }
	};
};

void memory_cases()
{
	using E = modbus::tcp::Endpoint<Memory>;
	State state;
	{
		E endpoint{std::in_place, state};
		tcp_test::Capture capture;
		CHECK(tcp_test::bind(endpoint, capture));
		CHECK(!endpoint.make_message(1u, 2u, 3u, SIZE_MAX) && state.tx_calls == 0u);
		state.short_tx = true;
		CHECK(!endpoint.make_message(1u, 2u, 3u, 0u) && state.live() == 0u);
		state.short_tx = false;
		auto message = endpoint.make_message(1u, 2u, 3u, 0u);
		CHECK(message && message.capacity() == 0u);
		CHECK(message.append_be(uint16_t{0xABCDu}));
		const auto capacity = message.capacity();
		state.fail_tx = true;
		CHECK(!message.reserve(100u) && message.size() == 2u && message.capacity() == capacity);
		const uint16_t values[]{0x1234u, 0x5678u};
		CHECK(!message.append_be(std::span<const uint16_t>{values}) && message.size() == 2u);
		state.fail_tx = false;
		state.short_tx = true;
		CHECK(!message.reserve(100u) && message.size() == 2u && state.live() == 1u);
		state.short_tx = false;
		state.overgrant = 1024u;
		CHECK(message.reserve(100u) && message.capacity() == E::max_send_size);
		CHECK(message.append_be(std::span<const uint16_t>{values}));
		CHECK(message.append_le(std::span<const uint16_t>{values}));
		CHECK(message.append_native(std::span<const uint16_t>{values}));
		CHECK(message.append_native(uint32_t{0x98765432u}));
		typename E::Message moved = std::move(message);
		CHECK(!message && message.size() == 0u && message.capacity() == 0u && moved.size() == 18u);
		message = std::move(moved);
		State foreign_state;
		E foreign{std::in_place, foreign_state};
		CHECK(foreign.send(message) == wire::SendResult::Invalid && message);
		CHECK(endpoint.send(message) == wire::SendResult::Sent && state.live() == 1u);
		endpoint.consume(capture.view());
		auto packet = endpoint.pop_packet();
		std::size_t offset = 0u;
		uint16_t a = 0u, b = 0u;
		CHECK(modbus::tcp::read_be(packet.data(), offset, a) && a == 0xABCDu);
		CHECK(modbus::tcp::read_be(packet.data(), offset, a) && a == 0x1234u);
		CHECK(modbus::tcp::read_be(packet.data(), offset, b) && b == 0x5678u);
		CHECK(modbus::tcp::read_le(packet.data(), offset, a) && a == 0x1234u);
		CHECK(modbus::tcp::read_le(packet.data(), offset, b) && b == 0x5678u);
		CHECK(modbus::tcp::read_native(packet.data(), offset, a) && a == 0x1234u);
		CHECK(modbus::tcp::read_native(packet.data(), offset, b) && b == 0x5678u);
		uint32_t last = 0u;
		CHECK(modbus::tcp::read_native(packet.data(), offset, last) && last == 0x98765432u);
		CHECK(!modbus::tcp::read_be(packet.data(), offset, last) && last == 0x98765432u && offset == packet.size());
		packet.reset();
		capture.active = false;
		endpoint.poll(0u);
		CHECK(state.live() == 0u);

		// OOM consumes exactly the declared tail even if it contains MBAP-like bytes.
		state.fail_rx = true;
		endpoint.consume(capture.view().first(6u));
		CHECK(endpoint.assembling() && endpoint.stats().rx.allocation_failure == 1u);
		state.fail_rx = false;
		endpoint.consume(capture.view().subspan(6u, 1u));
		endpoint.consume(capture.view().subspan(7u));
		CHECK(!endpoint.has_packet() && !endpoint.assembling() && !endpoint.rx_failed());
		CHECK(endpoint.stats().rx.skipped_frames == 1u && state.live() == 0u);
		endpoint.consume(capture.view());
		CHECK(endpoint.has_packet() && state.live() == 1u);
		endpoint.consume(capture.view().first(7u));
		CHECK(state.live() == 2u);
		endpoint.reset_rx();
		CHECK(state.live() == 1u && endpoint.has_packet());
		const unsigned calls = state.rx_calls;
		const uint8_t invalid[]{0, 0, 0, 1, 0, 2};
		endpoint.consume(invalid);
		CHECK(endpoint.rx_failed() && state.rx_calls == calls);
		endpoint.reset_rx();
		endpoint.consume(capture.view().first(7u));
		CHECK(state.live() == 2u); // destructor releases queued AND partial RX
	}
	CHECK(state.live() == 0u);
}

struct Sum : crc::Codec<uint16_t> {
	unsigned* calls;
	Sum() = delete;
	explicit Sum(unsigned& count) noexcept : calls(&count) {}
	uint16_t calculate(std::span<const uint8_t> bytes) noexcept
	{
		++*calls;
		uint16_t result = 0u;
		for (auto byte : bytes) { result = static_cast<uint16_t>(result + byte); }
		return result;
	}
};

void policy_cases()
{
	using E = modbus::tcp::Endpoint<Memory, modbus::tcp::Format<Sum, 32u>>;
	static_assert(!std::default_initializable<E>);
	State state;
	unsigned calls = 0u;
	{
		E endpoint{Sum{calls}, std::in_place, state};
		tcp_test::Capture capture;
		CHECK(tcp_test::bind(endpoint, capture));
		auto message = endpoint.make_message(0x100u, 2u, 3u, 0u);
		CHECK(message.append_native(uint8_t{7u}));
		capture.accept = false;
		CHECK(endpoint.send(message) == wire::SendResult::Failed && calls == 1u);
		CHECK(capture.size == 11u && capture.bytes[5] == 5u && capture.bytes[9] == 18u && capture.bytes[10] == 0u);
		capture.accept = true;
		CHECK(endpoint.send(message) == wire::SendResult::Sent && calls == 1u);
		endpoint.consume(capture.view());
		CHECK(calls == 2u && endpoint.pop_packet());
		capture.active = false;
		endpoint.poll(0u);
	}
	CHECK(state.live() == 0u);

	using B = modbus::tcp::Endpoint<wire::Pool<2, 2>, modbus::tcp::Format<crc::Crc16Bitwise>>;
	using T = modbus::tcp::Endpoint<wire::Pool<2, 2>, modbus::tcp::Format<crc::Crc16Table>>;
	static_assert(std::same_as<B::Storage, T::Storage> && std::same_as<B::Message, T::Message> && std::same_as<B::Packet, T::Packet>);
	static_assert(sizeof(B) == sizeof(T));
	static_assert(sizeof(B::Packet) == sizeof(void*));
	static_assert(!std::copy_constructible<B::Message> && std::copy_constructible<B::Packet>);
	static_assert(std::same_as<B::SendResult, cobs::SendResult> && std::same_as<B::SendResult, modbus::rtu::SendResult>);
	static_assert(&modbus::tcp::read_be<uint16_t> == &cobs::read_be<uint16_t>);
	static_assert(&modbus::tcp::read_be<uint16_t> == &modbus::rtu::read_be<uint16_t>);
	static_assert(B::Geometry::rx_block_bytes % B::Geometry::alignment == 0u);
	static_assert(B::max_send_size == 252u && B::max_receive_size == 252u);
	static_assert(B::Geometry::tx_block_bytes == 262u);
	static_assert(!std::move_constructible<B>);
}

void pool_oom()
{
	modbus::tcp::Endpoint<wire::Pool<1, 1>> endpoint;
	const uint8_t frame[]{0, 1, 0, 0, 0, 2, 1, 0x7F};
	endpoint.consume(frame);
	auto held = endpoint.pop_packet();
	endpoint.consume(frame);
	CHECK(held && !endpoint.has_packet() && endpoint.stats().rx.allocation_failure == 1u && endpoint.stats().rx.skipped_frames == 1u);
	held.reset();
	endpoint.consume(frame);
	CHECK(endpoint.pop_packet());
	auto message = endpoint.make_message(1u, 1u, 1u);
	CHECK(message && !endpoint.make_message(1u, 1u, 1u));
	message = {};
	CHECK(endpoint.make_message(1u, 1u, 1u));
}

uint32_t seed = 0x72F032D1u;
uint32_t random32() { seed ^= seed << 13u; seed ^= seed >> 17u; seed ^= seed << 5u; return seed; }
template<class Crc, std::size_t MaxData>
void stream_properties()
{
	using E = modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<Crc, MaxData>>;
	E endpoint;
	std::array<uint8_t, E::max_frame_size> bytes{};
	std::array<uint8_t, E::max_send_size> data{};
	for (unsigned iteration = 0u; iteration < 2000u; ++iteration) {
		const std::size_t length = random32() % (data.size() + 1u);
		for (auto& byte : data) { byte = static_cast<uint8_t>(random32()); }
		const auto transaction = static_cast<uint16_t>(random32());
		const auto unit = static_cast<uint8_t>(random32());
		const auto function = static_cast<uint8_t>(random32());
		const auto total = tcp_test::frame<Crc>(bytes.data(), transaction, unit, function, {data.data(), length});
		std::size_t offset = 0u;
		while (offset < total) {
			const auto count = std::min<std::size_t>(total - offset, 1u + random32() % 83u);
			endpoint.consume({bytes.data() + offset, count});
			offset += count;
		}
		auto packet = endpoint.pop_packet();
		CHECK(packet && packet.transaction_id() == transaction && packet.unit_id() == unit && packet.function() == function);
		CHECK(packet.size() == length && std::equal(packet.data().begin(), packet.data().end(), data.begin()));
		CHECK(!endpoint.has_packet() && !endpoint.assembling() && !endpoint.rx_failed());
	}
}

void extremes()
{
	using E = modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<crc::NoCrc, 65533u>>;
	E endpoint;
	std::vector<uint8_t> bytes(65541u, 0xADu);
	tcp_test::be16(bytes.data(), 0xFFFEu);
	bytes[2] = 0u; bytes[3] = 0u; bytes[4] = 0xFFu; bytes[5] = 0xFFu;
	endpoint.consume(bytes);
	auto packet = endpoint.pop_packet();
	CHECK(packet && packet.adu().size() == 65541u && packet.size() == 65533u && packet.transaction_id() == 0xFFFEu);
	std::vector<uint8_t> sent;
	CHECK(endpoint.bind([&sent](std::span<const uint8_t> b) noexcept { sent.assign(b.begin(), b.end()); return true; }, []() noexcept { return false; }));
	auto message = endpoint.make_message(0xFFFEu, 0xADu, 0xADu, 0u);
	CHECK(message.append_bytes(packet.data()) && !message.append_native(uint8_t{0u}));
	CHECK(endpoint.send(message) == wire::SendResult::Sent && sent == bytes);
	endpoint.poll(0u);
	modbus::tcp::Endpoint<wire::Pool<1, 1>, modbus::tcp::Format<crc::NoCrc, 0u>> small;
	CHECK(small.make_message(0u, 0u, 0u) && !small.make_message(0u, 0u, 0u, 1u));
	modbus::tcp::Endpoint<wire::Pool<1, 1>, modbus::tcp::Format<crc::Crc64Table, 0u>> trailer_only;
	CHECK(trailer_only.make_message(0u, 0u, 0u));

	// Hostile input remains bounded; only explicit reset grants a new boundary.
	modbus::tcp::Endpoint<wire::Pool<2, 1>> hostile;
	std::array<uint8_t, 89u> noise{};
	for (unsigned i = 0u; i < 10000u; ++i) {
		for (auto& byte : noise) { byte = static_cast<uint8_t>(random32()); }
		hostile.consume(noise);
		while (hostile.has_packet()) { auto p = hostile.pop_packet(); CHECK(p.adu().size() >= 8u && p.adu().size() <= 260u); }
		if ((i & 3u) == 0u) { hostile.reset_rx(); }
	}
	CHECK(!hostile.has_packet());
}
} // namespace
int main()
{
	memory_cases(); policy_cases(); pool_oom(); extremes();
	stream_properties<crc::NoCrc, 252u>();
	stream_properties<crc::Crc16Table, 252u>();
	stream_properties<crc::Crc32Bitwise, 1024u>();
	std::printf("TCP advanced: %u checks (memory faults, ownership, parity, stateful sum, extremes, seeded streams)\n", checks);
}
