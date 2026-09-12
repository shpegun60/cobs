/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Compile-time admission tests plus real calls through every protocol endpoint.
#ifndef WIRE_TEST_EXTENSION_CHECKS_H_
#define WIRE_TEST_EXTENSION_CHECKS_H_

#include "contract_checks.h"

namespace extension_checks {

// const& is a legitimate implementation of the by-value-shaped contract.
struct ConstRefOperations {
	std::byte* acquire_rx(const std::size_t&) noexcept;
	wire::TxBlock acquire_tx(const std::size_t&) noexcept;
	void release_rx(std::byte* const&) noexcept;
	void release_tx(const wire::TxBlock&) noexcept;
};
template<class Arg> struct BadAcquireRx : ConstRefOperations {
	using ConstRefOperations::acquire_rx;
	std::byte* acquire_rx(Arg) noexcept(false);
};
template<class Arg> struct BadAcquireTx : ConstRefOperations {
	using ConstRefOperations::acquire_tx;
	wire::TxBlock acquire_tx(Arg) noexcept(false);
};
template<class Arg> struct BadReleaseRx : ConstRefOperations {
	using ConstRefOperations::release_rx;
	void release_rx(Arg) noexcept(false);
};
template<class Arg> struct BadReleaseTx : ConstRefOperations {
	using ConstRefOperations::release_tx;
	void release_tx(Arg) noexcept(false);
};
static_assert(wire::ByteStorage<ConstRefOperations>);
static_assert(!wire::ByteStorage<BadAcquireRx<const std::size_t&>>);
static_assert(!wire::ByteStorage<BadAcquireRx<std::size_t&>>);
static_assert(!wire::ByteStorage<BadAcquireRx<std::size_t&&>>);
static_assert(!wire::ByteStorage<BadAcquireTx<const std::size_t&>>);
static_assert(!wire::ByteStorage<BadAcquireTx<std::size_t&>>);
static_assert(!wire::ByteStorage<BadAcquireTx<std::size_t&&>>);
static_assert(!wire::ByteStorage<BadReleaseRx<std::byte* const&>>);
static_assert(!wire::ByteStorage<BadReleaseRx<std::byte*&>>);
static_assert(!wire::ByteStorage<BadReleaseRx<std::byte*&&>>);
static_assert(!wire::ByteStorage<BadReleaseTx<const wire::TxBlock&>>);
static_assert(!wire::ByteStorage<BadReleaseTx<wire::TxBlock&>>);
static_assert(!wire::ByteStorage<BadReleaseTx<wire::TxBlock&&>>);

struct DeletedRelease : ConstRefOperations {
	using ConstRefOperations::release_rx;
	void release_rx(std::byte*&&) = delete;
};
struct WrongMutableRelease : ConstRefOperations {
	using ConstRefOperations::release_tx;
	int release_tx(wire::TxBlock&) noexcept;
};
struct RejectedMemory {
	template<class> using For = BadAcquireRx<std::size_t&&>;
};
static_assert(!wire::ByteStorage<DeletedRelease> && !wire::ByteStorage<WrongMutableRelease>);
static_assert(!wire::Storage<RejectedMemory, cobs::Endpoint<>::Geometry>);
static_assert(!wire::Storage<RejectedMemory, modbus::rtu::Endpoint<>::Geometry>);
static_assert(!wire::Storage<RejectedMemory, modbus::tcp::Endpoint<>::Geometry>);

template<class Memory = wire::Heap>
struct ConstRefMemory {
	template<class G> class For {
		typename Memory::template For<G> storage;
	public:
		std::byte* acquire_rx(const std::size_t& n) noexcept { return storage.acquire_rx(n); }
		wire::TxBlock acquire_tx(const std::size_t& n) noexcept { return storage.acquire_tx(n); }
		void release_rx(std::byte* const& p) noexcept { storage.release_rx(p); }
		void release_tx(const wire::TxBlock& b) noexcept { storage.release_tx(b); }
	};
};

namespace framing = modbus::rtu::framing;
using Direction = framing::Direction;
using Layout = framing::Layout;

struct ConstRefFramer {
	static constexpr Direction rx = Direction::Request;
	static constexpr Layout layout(const Direction& direction, const uint8_t& function) noexcept
	{
		return framing::standard_layout(direction, function);
	}
};
template<class D, class F> struct BadLayout : ConstRefFramer {
	using ConstRefFramer::layout;
	static Layout layout(D, F) noexcept(false);
};
struct BadMutableRx : ConstRefFramer {
	static inline Direction rx = Direction::Request;
	using ConstRefFramer::layout;
	static Layout layout(Direction&, const uint8_t&) noexcept(false);
};
struct BadRxConversion : ConstRefFramer {
	struct Rx { operator Direction() const noexcept(false); };
	static inline Rx rx{};
};
namespace adl {
struct Rx { operator Direction() const noexcept(false); };
Direction opposite(const Rx&) noexcept; // must NOT replace framing::opposite
}
struct BadAdlRxConversion : ConstRefFramer {
	using ConstRefFramer::layout;
	static inline adl::Rx rx{};
	static Layout layout(const adl::Rx&, const uint8_t&) noexcept;
};
struct DeletedLayout : ConstRefFramer {
	using ConstRefFramer::layout;
	static Layout layout(const Direction&, uint8_t&) = delete;
};
struct WrongLayout : ConstRefFramer {
	using ConstRefFramer::layout;
	static int layout(Direction&&, const uint8_t&) noexcept;
};
static_assert(framing::Policy<ConstRefFramer>);
static_assert(!framing::Policy<BadLayout<const Direction&, uint8_t&>>);
static_assert(!framing::Policy<BadLayout<Direction&&, const uint8_t&>>);
static_assert(!framing::Policy<BadMutableRx> && !framing::Policy<BadRxConversion>);
static_assert(!framing::Policy<BadAdlRxConversion>);
static_assert(!framing::Policy<DeletedLayout> && !framing::Policy<WrongLayout>);
static_assert(!framing::Framer<DeletedLayout> && framing::Framer<framing::None>);

// The same overloads are legal when ALL actual calls remain noexcept.
struct OverloadedFramer : ConstRefFramer {
	using ConstRefFramer::layout;
	inline static unsigned mutable_calls = 0u, temporary_calls = 0u;
	static Layout layout(const Direction& direction, uint8_t& function) noexcept
	{
		++mutable_calls;
		return ConstRefFramer::layout(direction, function);
	}
	static Layout layout(Direction&& direction, const uint8_t& function) noexcept
	{
		++temporary_calls;
		return ConstRefFramer::layout(direction, function);
	}
};
static_assert(framing::Policy<OverloadedFramer>);

template<class Framer, class Memory, class Check>
void framed_endpoint(Check check)
{
	using E = modbus::rtu::Endpoint<ConstRefMemory<Memory>, modbus::rtu::Format<crc::NoCrc>, Framer>;
	E endpoint;
	std::array<uint8_t, 8u> sent{};
	std::size_t size = 0u;
	check(endpoint.bind([&](const std::span<const uint8_t> bytes) noexcept {
		if (bytes.size() > sent.size()) { return false; }
		std::copy(bytes.begin(), bytes.end(), sent.begin()); size = bytes.size(); return true;
	}, []() noexcept { return false; }));
	const uint8_t request[]{1u, 3u, 0u, 0u, 0u, 1u};
	endpoint.consume(std::span<const uint8_t>{request}.first(1u));
	endpoint.consume(std::span<const uint8_t>{request}.subspan(1u));
	auto packet = endpoint.pop_packet();
	check(packet && packet.address() == 1u && packet.function() == 3u && packet.size() == 4u);
	endpoint.receive_adu(request); // const byte argument, unlike stream assembly
	check(static_cast<bool>(endpoint.pop_packet()));
	auto reply = endpoint.make_message(1u, 3u, 3u);
	const uint8_t data[]{2u, 0x12u, 0x34u};
	check(reply && reply.append_bytes(data));
	check(endpoint.send(reply) == wire::SendResult::Sent);
	const uint8_t expected[]{1u, 3u, 2u, 0x12u, 0x34u};
	check(std::ranges::equal(std::span<const uint8_t>{sent.data(), size}, expected));
	endpoint.poll(0u);
	check(!endpoint.tx_active());
}

template<class Memory = wire::Heap, class Check>
void run(Check check)
{
	contract_checks::endpoint<cobs::Endpoint<ConstRefMemory<Memory>>>(check);
	contract_checks::endpoint<modbus::rtu::Endpoint<ConstRefMemory<Memory>>>(check);
	contract_checks::endpoint<modbus::tcp::Endpoint<ConstRefMemory<Memory>>>(check);
	framed_endpoint<ConstRefFramer, Memory>(check);
	OverloadedFramer::mutable_calls = OverloadedFramer::temporary_calls = 0u;
	framed_endpoint<OverloadedFramer, Memory>(check);
	check(OverloadedFramer::mutable_calls != 0u && OverloadedFramer::temporary_calls != 0u);
}

} // namespace extension_checks
#endif
