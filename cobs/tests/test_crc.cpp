/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */
#include "Cobs.h"
#include "reference_frame.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what)
{
	++checks;
	if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}

struct Capture {
	bool active = false, accept = true;
	std::vector<uint8_t> frame;
	bool send(std::span<const uint8_t> bytes) noexcept
	{
		frame.assign(bytes.begin(), bytes.end());
		active = accept;
		return accept;
	}
	bool busy() const noexcept { return active; }
};
template<class E>
void bind(E& endpoint, Capture& capture)
{
	check(endpoint.bind(typename E::Sender{tiny::bind<&Capture::send>(capture)},
	                    typename E::BusyQuery{tiny::bind<&Capture::busy>(capture)}), "bind");
}

// Reference framing does not call the production encoder. CRC calculations
// have their own independent known-vector/property suite in crc/tests.
template<class Crc>
std::vector<uint8_t> body(std::span<const uint8_t> payload)
{
	std::vector<uint8_t> bytes(payload.begin(), payload.end());
	bytes.resize(payload.size() + Crc::wire_size);
	Crc crc;
	// Even a zero-length vector need not provide a pointer suitable for +0.
	auto* tail = bytes.data();
	if constexpr (Crc::wire_size != 0) { tail += payload.size(); }
	crc.store(tail, crc.calculate(payload));
	return bytes;
}

template<class Policy, std::size_t Maximum = cobs::Format<Policy>::max_send_size>
void round_trip()
{
	using F = cobs::Format<Policy, Maximum>;
	using E = cobs::Endpoint<wire::Heap, F>;
	E endpoint;
	Capture capture;
	bind(endpoint, capture);
	const std::array<std::size_t, 9> sizes{0u, 1u, 2u, 31u, 253u, 254u, 255u, 256u, Maximum};
	for (const auto n : sizes) {
		if (n > Maximum) { continue; }
		std::vector<uint8_t> payload(n);
		for (std::size_t i = 0; i < n; ++i) { payload[i] = static_cast<uint8_t>(i * 37u); }
		auto message = endpoint.make_message(0);
		check(message && message.size() == 0u, "empty builder");
		check(message.append_bytes(payload), "payload grows within CRC-adjusted limit");
		check(endpoint.send(message) == cobs::SendResult::Sent && !message, "send transfers ownership");
		const auto expected = cobs_test::frame(body<Policy>(payload), F::length_size);
		check(capture.frame == expected, "wire is length(body) + payload + policy trailer, independently COBS encoded");
		// Every split for short frames, small chunks for maximum/H2 frames.
		if (n <= 255u) {
			for (std::size_t cut = 0; cut <= expected.size(); ++cut) {
				endpoint.consume(std::span{expected}.first(cut));
				check(!endpoint.has_packet() || cut == expected.size(), "only delimiter publishes");
				endpoint.consume(std::span{expected}.subspan(cut));
				auto packet = endpoint.pop_packet();
				check(packet && std::ranges::equal(packet.data(), payload), "all splits publish payload only");
			}
		} else {
			for (std::size_t offset = 0; offset < expected.size(); offset += 37u) {
				endpoint.consume(std::span{expected}.subspan(offset, std::min<std::size_t>(37u, expected.size() - offset)));
			}
			auto packet = endpoint.pop_packet();
			check(packet && std::ranges::equal(packet.data(), payload), "large payload and CRC decode contiguously");
		}
		capture.active = false;
		endpoint.poll();
		check(!endpoint.tx_active(), "transport release");
	}
	check(!endpoint.make_message(Maximum + 1u), "hint excludes CRC and obeys explicit payload ceiling");
	check(endpoint.stats().rx.frames_lost == 0u, "valid frames have no loss");
}

struct Sum24 : crc::Codec<uint32_t, 3u, std::endian::little> {
	uint32_t calculate(std::span<const uint8_t> bytes) noexcept
	{
		uint32_t sum = 0;
		for (auto b : bytes) { sum = (sum + b) & 0xFFFFFFu; }
		return sum;
	}
};

struct LargeTrailer {
	using value_type = uint8_t;
	static constexpr std::size_t wire_size = 300u;
	uint8_t calculate(std::span<const uint8_t> bytes) noexcept
	{
		uint8_t sum = 0;
		for (auto b : bytes) { sum = static_cast<uint8_t>(sum + b); }
		return sum;
	}
	static void store(uint8_t* p, uint8_t sum) noexcept { std::memset(p, sum, wire_size); }
	static uint8_t load(const uint8_t* p) noexcept { return p[0]; }
};
static_assert(crc::Policy<LargeTrailer>);

struct State { unsigned calls = 0; const void* instance = nullptr; };
struct HardwareCrc : crc::Codec<uint16_t, 2u, std::endian::big> {
	State* state;
	HardwareCrc() = delete;
	explicit HardwareCrc(State& s) noexcept : state(&s) {}
	uint16_t calculate(std::span<const uint8_t> bytes) noexcept
	{
		++state->calls;
		state->instance = this;
		uint16_t sum = 0;
		for (auto b : bytes) { sum = static_cast<uint16_t>(sum + b); }
		return sum; // deliberately NOT CRC-16/MODBUS
	}
};

void stateful()
{
	using E = cobs::Endpoint<wire::Pool<2, 1>, cobs::Format<HardwareCrc>>;
	static_assert(!std::is_default_constructible_v<E>);
	State state;
	E endpoint{HardwareCrc{state}};
	Capture capture;
	bind(endpoint, capture);
	const std::array<uint8_t, 2> payload{0x41, 0x42};
	auto message = endpoint.make_message(0);
	check(message.append_bytes(payload), "stateful append");
	capture.active = true;
	check(endpoint.send(message) == cobs::SendResult::Busy && state.calls == 0u, "Busy never finalizes");
	capture.active = false;
	capture.accept = false;
	check(endpoint.send(message) == cobs::SendResult::Failed && state.calls == 1u, "failed start calculates once");
	const void* const instance = state.instance;
	const auto first = capture.frame;
	check(first == cobs_test::frame({0x41u, 0x42u, 0u, 0x83u}, 1u), "arbitrary sum and big-endian codec honored");
	check(!message.append_bytes(payload), "finalized retry is immutable");
	capture.accept = true;
	check(endpoint.send(message) == cobs::SendResult::Sent && state.calls == 1u && capture.frame == first,
	      "retry never calls stateful calculator again");
	endpoint.consume(capture.frame);
	auto packet = endpoint.pop_packet();
	check(packet && std::ranges::equal(packet.data(), payload) && state.calls == 2u && state.instance == instance,
	      "RX uses the exact TX calculator object and exposes no checksum bytes");
	capture.active = false;
	endpoint.poll();
}

struct Counter { unsigned acquired = 0; std::size_t requested = 0; };
struct ObservedHeap {
	template<class G> struct For {
		wire::Heap::For<G> heap;
		Counter& counter;
		explicit For(Counter& c) noexcept : counter(c) {}
		std::byte* acquire_rx(std::size_t bytes) noexcept
		{
			++counter.acquired;
			counter.requested = bytes;
			return heap.acquire_rx(bytes);
		}
		void release_rx(std::byte* p) noexcept { heap.release_rx(p); }
		wire::TxBlock acquire_tx(std::size_t bytes) noexcept { return heap.acquire_tx(bytes); }
		void release_tx(wire::TxBlock block) noexcept { heap.release_tx(block); }
	};
};

void rejection()
{
	Counter counter;
	using E = cobs::Endpoint<ObservedHeap>;
	E endpoint{std::in_place, counter};
	// Header alone, or a body shorter than the policy trailer: no allocation.
	endpoint.consume(cobs_test::frame({}, 1u));
	endpoint.consume(cobs_test::frame({0x42u}, 1u));
	check(!endpoint.has_packet() && counter.acquired == 0u && endpoint.stats().rx.length_mismatch == 2u,
	      "truncated trailer rejected before allocation");
	const std::array<uint8_t, 2> payload{0x41u, 0x42u};
	const auto good_body = body<crc::Crc16Bitwise>(payload);
	check(good_body == std::vector<uint8_t>({0x41u, 0x42u, 0xB1u, 0xD1u}), "locked independent AB CRC vector");
	const auto good = cobs_test::frame(good_body, 1u);
	const auto resyncs_before_crc = endpoint.stats().rx.resyncs;
	for (std::size_t bit = 0; bit < good_body.size() * 8u; ++bit) {
		auto bad_body = good_body;
		bad_body[bit / 8u] ^= static_cast<uint8_t>(1u << (bit % 8u));
		auto stream = cobs_test::frame(bad_body, 1u);
		stream.insert(stream.end(), good.begin(), good.end());
		endpoint.consume(stream);
		auto packet = endpoint.pop_packet();
		check(packet && std::ranges::equal(packet.data(), payload) && !endpoint.has_packet(),
		      "CRC rejection loses exactly one frame, next frame needs no extra delimiter");
	}
	check(endpoint.stats().rx.crc_errors == 32u && endpoint.stats().rx.resyncs == resyncs_before_crc,
	      "all payload/trailer single-bit errors counted, no spurious resync");
	check(counter.requested == sizeof(cobs::RxBlock<E::Storage>) + payload.size() + 2u,
	      "RX requests exact header + body, not alignment-rounded maximum");
	using Legacy = cobs::Endpoint<wire::Heap, cobs::Format<crc::NoCrc, 255>>;
	Legacy legacy;
	legacy.consume(good);
	auto packet = legacy.pop_packet();
	check(packet && std::ranges::equal(packet.data(), good_body), "v2 -> v1 can expose trailer as data: no autodetection");
	// The reverse is not guaranteed to fail for every payload; this vector does.
	endpoint.consume(cobs_test::frame({payload.begin(), payload.end()}, 1u));
	check(!endpoint.has_packet() && endpoint.stats().rx.crc_errors == 33u, "v1 AB -> v2 CRC failure");
}
} // namespace

int main()
{
	round_trip<crc::NoCrc>();
	round_trip<crc::Crc8Bitwise>();
	round_trip<crc::Crc8Table>();
	round_trip<crc::Crc16Bitwise>();
	round_trip<crc::Crc16Table>();
	round_trip<crc::Crc32Bitwise>();
	round_trip<crc::Crc32Table>();
	round_trip<crc::Crc64Bitwise>();
	round_trip<crc::Crc64Table>();
	round_trip<Sum24>();
	round_trip<LargeTrailer, 200u>();
	round_trip<crc::Crc16Bitwise, 0u>();
	round_trip<crc::Crc16Table, 254u>(); // H1/H2 threshold includes CRC
	round_trip<crc::Crc16Table, 65533u>();
	round_trip<crc::Crc64Table, 65527u>();
	stateful();
	rejection();
	std::printf("COBS integrity: %d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
