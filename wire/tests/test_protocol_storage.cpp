/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

// One custom memory specification, unchanged, used through both public APIs.
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <new>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* label)
{
	if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}

enum class Grant { Exact, Excess, Short };
struct State {
	Grant next = Grant::Exact;
	unsigned acquired = 0, released = 0, live = 0, errors = 0;
};

struct Memory {
	template<class G>
	class For {
		struct Allocation { std::byte* memory{}; std::size_t bytes{}; bool tx{}; };
		State& state;
		std::array<Allocation, 8> live{};

		std::byte* acquire(std::size_t bytes, bool tx) noexcept
		{
			for (auto& a : live) {
				if (a.memory != nullptr) { continue; }
				a = {static_cast<std::byte*>(::operator new(bytes, std::nothrow)), bytes, tx};
				if (a.memory != nullptr) { ++state.acquired; ++state.live; }
				return a.memory;
			}
			return nullptr;
		}
		void release(std::byte* p, std::size_t bytes, bool tx) noexcept
		{
			if (p == nullptr) { return; }
			for (auto& a : live) {
				if (a.memory != p) { continue; }
				if (a.tx != tx || (tx && a.bytes != bytes)) { ++state.errors; }
				::operator delete(p);
				a = {};
				++state.released;
				--state.live;
				return;
			}
			++state.errors;
		}
	public:
		explicit For(State& s) noexcept : state(s) {}
		std::byte* acquire_rx(std::size_t bytes) noexcept
		{
			return bytes <= G::rx_block_bytes ? acquire(bytes, false) : nullptr;
		}
		void release_rx(std::byte* p) noexcept { release(p, 0, false); }
		wire::TxBlock acquire_tx(std::size_t bytes) noexcept
		{
			if (bytes > G::tx_block_bytes) { return {}; }
			const auto mode = std::exchange(state.next, Grant::Exact);
			const auto granted = mode == Grant::Excess ? G::tx_block_bytes + 31u :
			                     mode == Grant::Short ? bytes - 1u : bytes;
			auto* p = acquire(granted, true);
			return p != nullptr ? wire::TxBlock{p, granted} : wire::TxBlock{};
		}
		void release_tx(wire::TxBlock block) noexcept
		{
			release(block.memory, block.granted, true);
		}
	};
};

struct Capture {
	std::vector<uint8_t> frame;
	bool borrowed = false;
	bool send(std::span<const uint8_t> bytes) noexcept
	{
		frame.assign(bytes.begin(), bytes.end());
		borrowed = true;
		return true;
	}
	bool busy() const noexcept { return borrowed; }
};

template<class E>
auto make(E& e, std::size_t hint)
{
	if constexpr (requires { e.make_message(hint); }) { return e.make_message(hint); }
	else { return e.make_message(1u, 3u, hint); }
}

template<class E>
void receive(E& e, std::span<const uint8_t> bytes)
{
	if constexpr (requires { e.consume(bytes); }) { e.consume(bytes); }
	else { e.receive_adu(bytes); }
}

template<class E>
void exercise()
{
	State state;
	{
		E endpoint{std::in_place, state};
		Capture capture;
		using Result = decltype(endpoint.send(std::declval<typename E::Message&>()));
		check(endpoint.bind(typename E::Sender{tiny::bind<&Capture::send>(capture)},
		                    typename E::BusyQuery{tiny::bind<&Capture::busy>(capture)}), "bind");
		state.next = Grant::Short;
		check(!make(endpoint, 8u) && state.live == 0u && state.released == 1u,
		      "undersized initial grant is returned and refused");
		{
			state.next = Grant::Excess;
			auto message = make(endpoint, 1u);
			check(message && message.capacity() == E::max_send_size,
			      "overgrant beyond Geometry is allowed but payload stays capped");
			std::vector<uint8_t> payload(E::max_send_size, 0xA5u);
			check(message.append_bytes(payload), "full capped capacity is writable");
			check(!message.reserve(E::max_send_size + 1u), "overgrant cannot enlarge wire limit");
			check(endpoint.send(message) == Result::Sent, "overgrant sends");
			check(state.live == 1u, "borrow keeps original allocation alive");
			receive(endpoint, capture.frame);
			auto packet = endpoint.pop_packet();
			check(packet && std::ranges::equal(packet.data(), payload), "overgrant wire round trip");
			auto held = packet;
			packet.reset();
			check(state.live == 2u, "shared packet retains RX allocation");
			capture.borrowed = false;
			endpoint.poll();
			check(state.live == 1u && state.errors == 0u, "transport returns original large descriptor");
			held.reset();
		}
		{
			auto message = make(endpoint, 8u);
			const std::array<uint8_t, 4> payload{0u, 1u, 0u, 3u};
			check(message.append_bytes(payload), "initial append");
			const auto capacity = message.capacity();
			const auto released = state.released;
			state.next = Grant::Short;
			check(!message.reserve(capacity + 8u) && message.capacity() == capacity &&
			      message.size() == payload.size() && state.live == 1u &&
			      state.released == released + 1u, "failed growth preserves old block and returns bad grant");
			check(message.reserve(capacity + 8u), "growth retries successfully");
			check(endpoint.send(message) == Result::Sent, "grown message sends");
			receive(endpoint, capture.frame);
			auto packet = endpoint.pop_packet();
			check(packet && std::ranges::equal(packet.data(), payload), "growth preserves logical bytes at new offset");
			capture.borrowed = false;
			endpoint.poll();
		}
	}
	check(state.live == 0u && state.acquired == state.released && state.errors == 0u,
	      "all descriptors returned unchanged to issuing instance");
}
} // namespace

int main()
{
	exercise<cobs::Endpoint<Memory>>();
	exercise<modbus::rtu::Endpoint<Memory>>();

	std::printf("Shared custom-memory protocol tests: %d failures\n", failures);
	return failures == 0 ? 0 : 1;
}
