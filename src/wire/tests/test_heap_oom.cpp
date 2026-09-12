/* Author: shpegun60; SPDX-License-Identifier: MIT */
// ELF linker interposition exercises the REAL Heap implementation, including
// protocol ownership. No production test hooks or replacement global new.
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>

extern "C" void* __real_malloc(std::size_t);
extern "C" void __real_free(void*);

namespace {
bool refuse = false;
std::size_t attempts = 0u, last_request = 0u, live_count = 0u;
std::array<void*, 64u> live{};
int failures = 0;
void check(bool value, const char* what)
{
	if (!value) { ++failures; std::printf("FAIL Heap OOM: %s\n", what); }
}
}
extern "C" void* __wrap_malloc(const std::size_t size)
{
	++attempts; last_request = size;
	if (refuse) { return nullptr; }
	void* const p = __real_malloc(size);
	if (p) {
		auto slot = std::find(live.begin(), live.end(), nullptr);
		if (slot == live.end()) { std::abort(); }
		*slot = p; ++live_count;
	}
	return p;
}
extern "C" void __wrap_free(void* p)
{
	if (p) {
		auto slot = std::find(live.begin(), live.end(), p);
		if (slot == live.end()) { std::abort(); }
		*slot = nullptr; --live_count;
	}
	__real_free(p);
}

namespace {
void requests()
{
	using G = wire::BlockGeometry<128u, 128u, alignof(std::max_align_t)>;
	wire::Heap::For<G> storage;
	for (const std::size_t size : {0u, 1u, 7u, 64u, 128u}) {
		auto* const p = storage.acquire_rx(size);
		check(p && last_request == std::max(size, G::alignment), "RX minimum physical size");
		check(reinterpret_cast<std::uintptr_t>(p) % G::alignment == 0u, "RX alignment");
		storage.release_rx(p);
		const auto t = storage.acquire_tx(size);
		check(t.memory && t.granted == size && last_request == (size ? size : 1u), "TX zero normalization/exact grant");
		storage.release_tx(t);
	}
	const auto before = attempts;
	check(!storage.acquire_rx(129u) && !storage.acquire_tx(129u).memory && attempts == before,
	      "oversize requests never call malloc");
	refuse = true;
	for (const std::size_t size : {0u, 1u, 128u}) {
		check(!storage.acquire_rx(size), "RX malloc failure returns null");
		const auto tx = storage.acquire_tx(size);
		check(!tx.memory && tx.granted == 0u, "TX malloc failure returns empty descriptor");
	}
	refuse = false;
	storage.release_rx(nullptr); storage.release_tx({});
	check(live_count == 0u, "raw allocation ownership balances");
}

struct Capture {
	std::array<uint8_t, 512u> data{};
	std::size_t size = 0u;
	bool held = false;
	bool send(std::span<const uint8_t> bytes) noexcept
	{
		if (held || bytes.size() > data.size()) { return false; }
		std::copy(bytes.begin(), bytes.end(), data.begin()); size = bytes.size(); held = true; return true;
	}
	bool busy() const noexcept { return held; }
	std::span<const uint8_t> bytes() const noexcept { return {data.data(), size}; }
};
namespace framing = modbus::rtu::framing;
struct PrivateFramer : framing::Standard<framing::Direction::Request> {
	static constexpr framing::Layout layout(framing::Direction direction, uint8_t function) noexcept
	{
		return function == 0x41u ? framing::Layout::length_prefixed(2u)
		                         : framing::standard_layout(direction, function);
	}
};
using Framed = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>, PrivateFramer>;
template<class E> auto make(E& endpoint, std::size_t hint)
{
	if constexpr (requires { endpoint.make_message(hint); }) { return endpoint.make_message(hint); }
	else if constexpr (E::framed) { return endpoint.make_message(0x11u, 0x41u, hint + 2u); }
	else { return endpoint.make_message(0x11u, 3u, hint); }
}
template<class E> std::span<const uint8_t> application(const typename E::Packet& packet)
{
	auto data = packet.data();
	if constexpr (std::is_same_v<E, Framed>) {
		if (data.size() < 2u || ((static_cast<std::size_t>(data[0]) << 8u) | data[1]) != data.size() - 2u) {
			check(false, "framed RTU owned length prefix"); return {};
		}
		return data.subspan(2u);
	} else { return data; }
}
template<class E> void receive(E& endpoint, std::span<const uint8_t> bytes)
{
	if constexpr (requires { endpoint.consume(bytes); }) { endpoint.consume(bytes); }
	else { endpoint.receive_adu(bytes); }
}
template<class E> void protocol()
{
	const auto initial_live = live_count;
	{
		E endpoint;
		Capture capture;
		check(endpoint.bind(typename E::Sender{tiny::bind<&Capture::send>(capture)},
		                    typename E::BusyQuery{tiny::bind<&Capture::busy>(capture)}), "bind");
		refuse = true;
		check(!make(endpoint, 32u) && live_count == initial_live, "failed message construction owns no memory");
		refuse = false;
		auto message = make(endpoint, 2u);
		constexpr std::array<uint8_t, 2> prefix{0x41u, 0x42u};
		constexpr std::array<uint8_t, 32> tail{0x11u, 0x22u, 0x33u, 0x44u};
		check(message && message.append_bytes(prefix), "initial prefix");
		const auto capacity = message.capacity(), size = message.size();
		refuse = true;
		check(!message.reserve(34u) && !message.append_bytes(tail) &&
		      !message.append_native(uint32_t{1u}) && !message.append_be(uint32_t{1u}) &&
		      !message.append_le(uint32_t{1u}), "all growth entry points refuse malloc failure");
		check(message && message.size() == size && message.capacity() == capacity && live_count == initial_live + 1u,
		      "failed growth preserves size, capacity and owner");
		refuse = false;
		check(message.append_bytes(tail), "same message retries growth after memory becomes available");
		check(endpoint.send(message) == wire::SendResult::Sent && !message, "recovered message sends");
		check(live_count == initial_live + 1u && endpoint.tx_active(), "TX borrow owns memory");
		receive(endpoint, capture.bytes());
		auto original = endpoint.pop_packet();
		auto old_copy = original;
		check(original && application<E>(original).size() == 34u, "pre-OOM Packet retained");
		const auto before_rx = endpoint.stats().rx;
		refuse = true;
		for (unsigned i = 0u; i < 3u; ++i) { receive(endpoint, capture.bytes()); }
		check(!endpoint.has_packet() && endpoint.stats().rx.allocation_failure == before_rx.allocation_failure + 3u &&
		      endpoint.stats().rx.frames_received == before_rx.frames_received &&
		      endpoint.stats().rx.crc_errors == before_rx.crc_errors,
		      "RX failure is counted without an invalid packet");
		check(original && old_copy && std::ranges::equal(application<E>(original), application<E>(old_copy)),
		      "both old Packet references survive OOM");
		refuse = false;
		receive(endpoint, capture.bytes());
		auto packet = endpoint.pop_packet();
		const auto data = packet ? application<E>(packet) : std::span<const uint8_t>{};
		check(packet && data.size() == 34u &&
		      std::ranges::equal(data.first(2u), prefix) &&
		      std::ranges::equal(data.subspan(2u), tail), "prefix and full body survive failed growth and RX recovery");
		check(old_copy && std::ranges::equal(application<E>(old_copy), data), "retained Packet still contains the exact old data");
		original.reset(); old_copy.reset();
		auto retained = packet; packet.reset();
		capture.held = false; endpoint.poll(0u);
		check(live_count == initial_live + 1u, "only retained Packet remains after TX completion");
		retained.reset();
		check(live_count == initial_live && !endpoint.tx_active(), "RX and TX allocation/free pairs balance");
	}
	check(live_count == initial_live, "endpoint destruction leaks no allocations");
}
}
int main()
{
	requests();
	protocol<cobs::Endpoint<>>();
	protocol<modbus::rtu::Endpoint<>>();
	protocol<Framed>();
	std::printf("Heap OOM: %d failures; %zu allocator calls; %zu live blocks\n", failures, attempts, live_count);
	return failures == 0 && live_count == 0u ? 0 : 1;
}
