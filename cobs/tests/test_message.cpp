/*
 * Host verification for cobs::Message: block ownership, public container semantics,
 * the growth rule and the serializers. Encoding is deliberately observed only
 * through the real Endpoint coordinator; applications cannot invoke its internal
 * state transitions directly.
 *
 * Two things are asserted only INDIRECTLY, on purpose:
 *
 *   - Ownership, through TX pool occupancy and through a counting policy. A
 *     message that leaks a block or frees one twice shows up as the wrong
 *     number of available blocks, never as a field read out of the object.
 *   - Payload contents, through Endpoint:.send() and the reference encoder.
 *     cobs::Message hands out no payload span or encoding hook, so a wrong byte, a
 *     lost byte or a botched growth copy surfaces at the coordinator boundary.
 */
#include "Cobs.h"
#include "reference_frame.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void group(const char* name) { std::printf("\n[%s]\n", name); }

void check(const bool ok, const std::string& what)
{
	++g_checks;
	if (ok) {
		std::printf("  ok    %s\n", what.c_str());
	} else {
		++g_failures;
		std::printf("  FAIL  %s\n", what.c_str());
	}
}

constexpr std::size_t kMaxDecoded = 64;
constexpr std::size_t kTxBlocks = 2; // recommended quota: build one while one flies

using TxPool = cobs::Pool<1, kTxBlocks, cobs::Format<kMaxDecoded>>;
using HeapPool = cobs::Heap<cobs::Format<kMaxDecoded>>;
using Message = cobs::Message<TxPool>;

// A message filled with `n` recognisable bytes, plus the bytes themselves for
// the oracle to check against.
template<class M>
std::vector<uint8_t> fill(M& m, const std::size_t n, const uint8_t tag)
{
	std::vector<uint8_t> expected(n);
	for (std::size_t i = 0; i < n; ++i) {
		expected[i] = static_cast<uint8_t>(tag + i);
	}
	if (!m.append_bytes(std::span<const uint8_t>{expected})) {
		expected.clear(); // the caller's assertion will notice
	}
	return expected;
}

struct CaptureTransport {
	std::vector<std::vector<uint8_t>> attempts;
	std::vector<std::vector<uint8_t>> accepted;
	bool busy = false;
	bool refuse = false;

	bool send(const std::span<const uint8_t> wire) noexcept
	{
		attempts.emplace_back(wire.begin(), wire.end());
		if (refuse) {
			return false;
		}
		accepted.push_back(attempts.back());
		busy = true;
		return true;
	}

	bool tx_busy() const noexcept { return busy; }
	void finish() noexcept { busy = false; }
};

template<class Engine>
bool bindTransport(Engine& endpoint, CaptureTransport& transport)
{
	return endpoint.bind(
		typename Engine::Sender{[&transport](const std::span<const uint8_t> wire) noexcept {
			return transport.send(wire);
		}},
		typename Engine::BusyQuery{[&transport]() noexcept {
			return transport.tx_busy();
		}});
}

template<class Engine>
bool sendsAs(Engine& endpoint, CaptureTransport& transport,
	         typename Engine::Message& message, const std::vector<uint8_t>& expected)
{
	const std::size_t before = transport.accepted.size();
	if (endpoint.send(message) != cobs::SendResult::Sent) {
		return false;
	}
	const bool matches = transport.accepted.size() == before + 1u &&
		transport.accepted.back() == cobs_test::frame(expected, Engine::length_size);
	transport.finish();
	endpoint.poll();
	return matches && !endpoint.tx_active();
}

/* ============================== ownership =============================== */

void testAcquireAndRelease()
{
	TxPool pool;
	check(pool.tx_available() == kTxBlocks, "the TX pool starts full");
	{
		Message m{pool};
		check(static_cast<bool>(m), "a message acquires a block");
		check(pool.tx_available() == kTxBlocks - 1, "which the pool records as in use");
		check(m.size() == 0, "and starts empty");
	}
	check(pool.tx_available() == kTxBlocks, "destroying a Building message returns the block");

	{	// A default-constructed message owns nothing and must not free anything.
		Message empty;
		check(!empty, "a default-constructed message is empty");
		check(empty.append_native<uint8_t>(1) == false, "and accepts no appends");
		check(empty.size() == 0 && empty.capacity() == 0, "and exposes zero geometry");
	}
	check(pool.tx_available() == kTxBlocks && pool.tx_stats().rejected == 0,
	      "an empty message frees nothing on destruction");
}

void testExhaustionAndConcurrentMessages()
{
	TxPool pool;
	Message a{pool};
	Message b{pool};
	check(static_cast<bool>(a) && static_cast<bool>(b),
	      "several messages can be held at once");
	check(pool.tx_available() == 0, "consuming the whole TX pool");

	Message c{pool};
	check(!c, "a further message from a dry pool is empty rather than a failure code");
	check(c.append_native<uint8_t>(0) == false, "and stays unusable");

	{	// Distinct blocks, or two messages would encode over each other.
		const auto ea = fill(a, 8, 0x10);
		const auto eb = fill(b, 8, 0x90);
		check(a.size() == ea.size() && b.size() == eb.size(),
		      "concurrent messages remain independently writable");
	}

	a = Message{};
	check(pool.tx_available() == 1, "assigning an empty message over one releases its block");
	Message d{pool};
	check(static_cast<bool>(d), "and the pool hands that block out again");
	check(pool.tx_stats().rejected == 0, "with no block freed twice");
}

void testMoveSemantics()
{
	TxPool pool;
	{
		Message a{pool};
		const auto expected = fill(a, 20, 0x20);

		Message b = std::move(a);
		check(!a, "a moved-from message is empty");
		check(static_cast<bool>(b) && b.size() == 20,
		      "and the destination carries the payload");
		check(pool.tx_available() == kTxBlocks - 1,
		      "the block moved rather than being duplicated or lost");
		check(b.size() == expected.size(),
		      "the moved message retains the complete logical payload");
	}
	check(pool.tx_available() == kTxBlocks, "and is released once");

	{	// Move assignment must release what it overwrites, not leak it.
		Message a{pool};
		Message b{pool};
		check(pool.tx_available() == 0, "two blocks held");
		b = std::move(a);
		check(pool.tx_available() == 1, "move assignment released b's own block first");
		check(!a && static_cast<bool>(b), "and transferred a's");

		// Self-move must not destroy the message.
		Message& alias = b;
		b = std::move(alias);
		check(static_cast<bool>(b), "self move-assignment leaves the message intact");
	}
	check(pool.tx_available() == kTxBlocks, "everything is back");
	check(pool.tx_stats().rejected == 0, "with nothing double-freed");

	// Exclusive ownership is a compile-time property, not a convention.
	static_assert(!std::is_copy_constructible_v<Message>, "cobs::Message must not be copyable");
	static_assert(!std::is_copy_assignable_v<Message>, "cobs::Message must not be copy-assignable");
	check(true, "copying is rejected at compile time");
}

/* ========================== container semantics ========================= */

/*
 * A counting policy: exact capacity like the heap one, but it records every
 * allocation and can be told to refuse the next. Growth is only observable
 * through the allocator, which is exactly right — a container that reallocates
 * is measured by how often it does so, not by its internal pointers.
 */
class SpyStorage final {
public:
	using Format = cobs::Format<8, 4096>;
	using RxBlock = cobs::RxBlock<SpyStorage>;
	[[nodiscard]] RxBlock* acquire_rx(std::size_t) noexcept { return nullptr; }
	void release_rx(RxBlock*) noexcept {}

	[[nodiscard]] cobs::TxBlock acquire_tx(const std::size_t requested) noexcept
	{
		if (requested > Format::max_send_size || refuse_next) {
			return {};
		}
		void* const memory = ::operator new(Format::tx_storage_size_for_capacity(requested), std::nothrow);
		if (memory == nullptr) {
			return {};
		}
		++allocations;
		last_request = requested;
		return {static_cast<std::byte*>(memory), requested};
	}

	void release_tx(const cobs::TxBlock block) noexcept
	{
		if (block.memory != nullptr) {
			++frees;
			last_freed_capacity = block.capacity;
		}
		::operator delete(static_cast<void*>(block.memory));
	}

	std::size_t allocations = 0;
	std::size_t frees = 0;
	std::size_t last_request = 0;
	std::size_t last_freed_capacity = 0;
	mutable bool refuse_next = false;
};

/*
 * A policy that over-allocates the way a segregated allocator would: neither
 * exact like the heap one nor "the whole limit" like the single-slab pool, but
 * somewhere in between. The two shipped policies only exercise the extremes,
 * and a container that had quietly assumed capacity == requested would pass
 * both of them.
 */
class SizeClassStorage final {
public:
	using Format = cobs::Format<8, 4096>;
	using RxBlock = cobs::RxBlock<SizeClassStorage>;
	[[nodiscard]] RxBlock* acquire_rx(std::size_t) noexcept { return nullptr; }
	void release_rx(RxBlock*) noexcept {}

	// Its own rule, obeying nothing but the contract: 2n + 1, capped.
	[[nodiscard]] static constexpr std::size_t class_for(const std::size_t n) noexcept
	{
		const std::size_t twice = n * 2u + 1u;
		return (twice > Format::max_send_size) ? Format::max_send_size : twice;
	}

	[[nodiscard]] cobs::TxBlock acquire_tx(const std::size_t requested) noexcept
	{
		if (requested > Format::max_send_size) {
			return {};
		}
		const std::size_t capacity = class_for(requested);
		void* const memory = ::operator new(Format::tx_storage_size_for_capacity(capacity), std::nothrow);
		if (memory == nullptr) {
			return {};
		}
		++allocations;
		return {static_cast<std::byte*>(memory), capacity};
	}

	void release_tx(const cobs::TxBlock block) noexcept
	{
		if (block.memory != nullptr) {
			++frees;
			last_freed_capacity = block.capacity;
		}
		::operator delete(static_cast<void*>(block.memory));
	}

	std::size_t allocations = 0;
	std::size_t frees = 0;
	std::size_t last_freed_capacity = 0;
};

// The container must work off the REPORTED capacity, never off what it asked
// for. This is the case between the two extremes the shipped policies cover.
void testIntermediateOverallocation()
{
	SizeClassStorage pool;
	{
		cobs::Message<SizeClassStorage> m{pool, 7};
		check(m.capacity() == 15, "a policy may grant more than was requested (7 -> 15)");
		check(m.size() == 0, "without putting anything in the message");

		// Fifteen bytes must fit with no reallocation, even though only seven
		// were ever asked for.
		const std::vector<uint8_t> body(15, 0x2A);
		check(m.append_bytes(std::span<const uint8_t>{body}), "the whole grant is usable");
		check(pool.allocations == 1, "with no reallocation");
		check(m.size() == body.size(), "and uses the whole reported geometry");
	}
	check(pool.frees == 1 && pool.last_freed_capacity == 15,
	      "the block goes back with the capacity the policy reported, not the 7 asked for");

	{	// A growth must also ask through the policy's rule, and record what
		// came back rather than what it wanted.
		SizeClassStorage grow;
		cobs::Message<SizeClassStorage> m{grow, 4};
		check(m.capacity() == 9, "4 -> 9");
		const std::vector<uint8_t> body(20, 0x3B);
		check(m.append_bytes(std::span<const uint8_t>{body}), "20 bytes need a growth");
		// target = max(20, 9 + 4) = 20; the policy answers 41.
		check(m.capacity() == 41, "the container asked for 20 and was granted 41");
		check(grow.allocations == 2 && grow.frees == 1, "in exactly one reallocation");
		check(grow.last_freed_capacity == 9, "the old block returned with ITS capacity");
		check(m.size() == body.size(), "and the logical payload survived the move");
	}
}

void testCreation()
{
	{	// The hint is a capacity hint, never an initial size.
		HeapPool heap;
		cobs::Message<HeapPool> unhinted{heap};
		check(static_cast<bool>(unhinted), "an unhinted message is valid");
		check(unhinted.size() == 0 && unhinted.capacity() == 0,
		      "heap: no hint means no capacity yet, and never any size");

		cobs::Message<HeapPool> hinted{heap, 40};
		check(hinted.size() == 0, "a hint does not put bytes in the message");
		check(hinted.capacity() == 40, "heap: the hint is honoured exactly");
	}
	{	// The single-slab pool answers with its whole limit either way, so a
		// message on it is born with all the room it will ever need.
		TxPool pool;
		Message unhinted{pool};
		check(unhinted.size() == 0 && unhinted.capacity() == kMaxDecoded,
		      "fixed: even an unhinted message gets the whole slab");
		Message hinted{pool, 8};
		check(hinted.capacity() == kMaxDecoded, "fixed: and a small hint changes nothing");
	}
	{	// A hint past the protocol limit is refused rather than clamped: the
		// caller asked for something this instance cannot carry.
		HeapPool heap;
		cobs::Message<HeapPool> over{heap, kMaxDecoded + 1};
		check(!over, "a hint beyond tx_max_size yields an empty message");
	}
}

void testGrowthSequence()
{
	SpyStorage spy;
	cobs::Message<SpyStorage> m{spy};
	check(m.capacity() == 0 && spy.allocations == 1,
	      "the initial block is one allocation, of no capacity");

	// One byte at a time: capacity must follow the documented 1.5x rule, and
	// must never fail to advance.
	std::vector<std::size_t> caps;
	bool writes_ok = true;
	for (std::size_t i = 0; i < 10; ++i) {
		writes_ok = writes_ok && m.append_native<uint8_t>(static_cast<uint8_t>(i));
		caps.push_back(m.capacity());
	}
	check(writes_ok, "ten single-byte appends all succeed");
	const std::vector<std::size_t> expected{1, 2, 3, 4, 6, 6, 9, 9, 9, 13};
	check(caps == expected, "capacity follows 0 -> 1 -> 2 -> 3 -> 4 -> 6 -> 9 -> 13");
	check(m.size() == 10, "and the size is the number of bytes actually appended");

	// Every growth is one allocation and one release; nothing accumulates.
	check(spy.frees == spy.allocations - 1,
	      "each growth released exactly the block it replaced");
	check(spy.last_freed_capacity == 9,
	      "and released it with ITS capacity, not the new one");

	check(m.size() == 10, "and every append survived the reallocations");
}

void testLargeJumpIsOneAllocation()
{
	SpyStorage spy;
	cobs::Message<SpyStorage> m{spy, 64};
	check(m.capacity() == 64 && spy.allocations == 1, "starting from a 64-byte hint");

	const std::vector<uint8_t> body(500, 0x5A);
	check(m.append_bytes(std::span<const uint8_t>{body}), "a 500-byte append succeeds");
	check(m.capacity() == 500,
	      "asking for exactly what is required, not walking 96 -> 144 -> 216");
	check(spy.allocations == 2 && spy.last_request == 500,
	      "in a single allocation");
	check(m.size() == body.size(), "and the complete payload is retained");
}

void testNoGrowthWhenItFits()
{
	SpyStorage spy;
	cobs::Message<SpyStorage> m{spy, 256};
	const std::size_t after_create = spy.allocations;

	bool ok = true;
	for (int i = 0; i < 64; ++i) { ok = ok && m.append_native<uint32_t>(0x11223344u); }
	check(ok, "256 bytes appended into a 256-byte capacity");
	check(m.size() == 256 && m.capacity() == 256, "filling it exactly");
	check(spy.allocations == after_create && spy.frees == 0,
	      "with no reallocation and no release at all");

	// And the fixed policy gets this for free, without any hint.
	TxPool pool;
	Message fixed{pool};
	bool fill_ok = true;
	for (std::size_t i = 0; i < kMaxDecoded; ++i) {
		fill_ok = fill_ok && fixed.append_native<uint8_t>(static_cast<uint8_t>(i));
	}
	check(fill_ok && fixed.size() == kMaxDecoded,
	      "fixed: a whole tx_max_size payload appended one byte at a time");
	check(pool.tx_available() == kTxBlocks - 1,
	      "still on the one block it started with — no growth ever happened");
	check(fixed.append_native<uint8_t>(0) == false, "and one byte past the limit is refused");
	check(fixed.size() == kMaxDecoded, "leaving the message exactly as it was");
}

void testFailedGrowthChangesNothing()
{
	SpyStorage spy;
	cobs::Message<SpyStorage> m{spy, 8};
	(void)fill(m, 8, 0x70);
	check(m.size() == 8 && m.capacity() == 8, "eight bytes in an eight-byte capacity");

	const std::size_t allocations = spy.allocations;
	const std::size_t frees = spy.frees;
	spy.refuse_next = true;

	check(m.append_native<uint32_t>(0xDEADBEEFu) == false,
	      "an append needing growth fails");
	check(m.size() == 8 && m.capacity() == 8, "leaving size and capacity untouched");
	check(spy.frees == frees, "and releasing nothing — the old block is still ours");
	check(spy.allocations == allocations, "with no allocation having succeeded");

	check(m.reserve(64) == false, "reserve fails the same way");
	check(m.size() == 8 && m.capacity() == 8, "and changes nothing either");

	spy.refuse_next = false;
	check(m.append_native<uint8_t>(0x71),
	      "the message remains writable after the failed growth");
}

void testReserve()
{
	SpyStorage spy;
	cobs::Message<SpyStorage> m{spy};
	check(m.reserve(0), "reserving nothing on an empty message succeeds");
	check(spy.allocations == 1, "without allocating");

	check(m.reserve(100), "reserve grows to the exact request");
	check(m.capacity() == 100, "not to a geometric target above it");
	check(spy.allocations == 2, "in one allocation");

	check(m.reserve(50), "a smaller reserve is a no-op");
	check(m.capacity() == 100 && spy.allocations == 2, "and does not shrink or reallocate");

	check(m.reserve(SpyStorage::Format::max_send_size + 1) == false,
	      "reserving past tx_max_size is refused");
	check(m.capacity() == 100, "leaving the capacity alone");
}

/* ============================== serializers ============================= */

enum class Op : uint16_t { Ping = 0x0102, Pong = 0x0304 };

void testSerializers()
{
	using Engine = cobs::Endpoint<SpyStorage>;
	Engine endpoint;
	CaptureTransport transport;
	check(bindTransport(endpoint, transport), "the serializer coordinator binds");
	auto m = endpoint.make_message(0);

	// Everything is appended in target byte order, so the oracle is built the
	// same way — through memcpy of the same objects, never by hand-guessing
	// an endianness.
	std::vector<uint8_t> expected;
	const auto expect = [&expected](const void* p, const std::size_t n) {
		const auto* const bytes = static_cast<const uint8_t*>(p);
		expected.insert(expected.end(), bytes, bytes + n);
	};

	bool ok = true;
	const uint8_t  u8  = 0x11;         ok = ok && m.append_native(u8);  expect(&u8, 1);
	const int8_t   i8  = -2;           ok = ok && m.append_native(i8);  expect(&i8, 1);
	const uint16_t u16 = 0x1234;       ok = ok && m.append_native(u16); expect(&u16, 2);
	const int16_t  i16 = -300;         ok = ok && m.append_native(i16); expect(&i16, 2);
	const uint32_t u32 = 0xDEADBEEFu;  ok = ok && m.append_native(u32); expect(&u32, 4);
	const int32_t  i32 = -70000;       ok = ok && m.append_native(i32); expect(&i32, 4);
	const uint64_t u64 = 0x0102030405060708ull;
	                                   ok = ok && m.append_native(u64); expect(&u64, 8);
	const int64_t  i64 = -1;           ok = ok && m.append_native(i64); expect(&i64, 8);
	const float    f   = 1.5f;         ok = ok && m.append_native(f);   expect(&f, sizeof f);
	const double   d   = -2.25;        ok = ok && m.append_native(d);   expect(&d, sizeof d);
	const Op       op  = Op::Pong;     ok = ok && m.append_native(op);  expect(&op, sizeof op);
	check(ok, "every scalar, enum and floating-point value appends");

	{	// A flag goes out as the byte the caller chose, because append_native<T> does
		// not accept bool at all (see cobs::detail::NativeScalar).
		const uint8_t flag = 1;
		ok = m.append_native(flag);
		expect(&flag, 1);
		const std::byte raw_byte{0xA5};
		ok = ok && m.append_native(raw_byte);
		expect(&raw_byte, 1);
		check(ok, "and std::byte goes through as itself");
	}
	{	// A byte span, then an array of a wider type.
		const std::vector<uint8_t> blob{0x00, 0xFF, 0x00, 0x7F};
		ok = m.append_bytes(std::span<const uint8_t>{blob});
		expect(blob.data(), blob.size());

		const uint16_t words[] = {0x0001, 0x0200, 0xFFFF};
		ok = ok && m.append_native(std::span<const uint16_t>{words});
		expect(words, sizeof words);
		check(ok, "so do a byte span and an array of a wider type");
	}

	check(m.size() == expected.size(), "the size is the sum of everything appended");
	check(sendsAs(endpoint, transport, m, expected),
	      "and the frame is exactly those bytes, canonically encoded (" +
	          std::to_string(expected.size()) + " payload bytes)");
}

void testSerializerFailuresLeaveTheMessageUsable()
{
	using Engine = cobs::Endpoint<SpyStorage>;
	Engine endpoint;
	CaptureTransport transport;
	check(bindTransport(endpoint, transport), "the failure-recovery coordinator binds");
	auto m = endpoint.make_message(4);
	check(m.append_native<uint32_t>(0x01020304u), "four bytes fit");

	endpoint.storage().refuse_next = true;
	check(m.append_native<uint64_t>(0) == false,
	      "an eight-byte append that cannot grow fails");
	const uint8_t blob[16] = {};
	check(m.append_bytes(std::span<const uint8_t>{blob}) == false, "so does a span");
	const uint32_t words[8] = {};
	check(m.append_native(std::span<const uint32_t>{words}) == false, "so does an array");
	check(m.size() == 4, "and none of them moved the size");

	endpoint.storage().refuse_next = false;
	check(m.append_native<uint8_t>(0x05), "the message still works once memory is available");
	check(m.size() == 5, "appending after a failed append, not on top of it");

	// The uint32 went out in target order, so build the oracle the same way.
	std::vector<uint8_t> oracle(5);
	const uint32_t v = 0x01020304u;
	std::memcpy(oracle.data(), &v, 4);
	oracle[4] = 0x05;
	check(sendsAs(endpoint, transport, m, oracle), "with the earlier bytes intact");
}

void testOversizeIsRefusedNotClamped()
{
	SpyStorage spy;
	cobs::Message<SpyStorage> m{spy};
	const std::vector<uint8_t> huge(SpyStorage::Format::max_send_size + 1, 0x33);
	check(m.append_bytes(std::span<const uint8_t>{huge}) == false,
	      "a payload past tx_max_size is refused");
	check(m.size() == 0 && spy.allocations == 1, "with nothing appended and nothing allocated");

	const std::vector<uint8_t> exact(SpyStorage::Format::max_send_size, 0x44);
	check(m.append_bytes(std::span<const uint8_t>{exact}),
	      "while exactly tx_max_size is accepted");
	check(m.size() == SpyStorage::Format::max_send_size && m.capacity() == SpyStorage::Format::max_send_size,
	      "filling the message to its limit");
	check(m.append_native<uint8_t>(0) == false, "after which nothing more fits");
}

/* ================================ encode ================================ */

void testCoordinatorEncoding()
{
	using Engine = cobs::Endpoint<TxPool>;
	Engine endpoint;
	CaptureTransport transport;
	check(bindTransport(endpoint, transport), "the real coordinator binds for encoding tests");

	// Every interesting length, cross-checked against the reference encoder.
	for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2},
	                            kMaxDecoded - 1, kMaxDecoded}) {
		auto m = endpoint.make_message(n);
		const auto expected = fill(m, n, 0x30);
		check(sendsAs(endpoint, transport, m, expected),
		      "payload of " + std::to_string(n) + " bytes encodes canonically in place");
	}

	{	// A failed start leaves an encoded block in the message until its lifetime ends.
		auto m = endpoint.make_message(8);
		(void)fill(m, 8, 0x20);
		transport.refuse = true;
		check(endpoint.send(m) == cobs::SendResult::Failed,
		      "a failed start leaves the encoded message owned by the caller");
		check(endpoint.storage().tx_available() == kTxBlocks - 1,
		      "and its block remains live");
	}
	check(endpoint.storage().tx_available() == kTxBlocks,
	      "destroying that encoded message returns the block");
	transport.refuse = false;

	{	// A refused start retains the private Encoded state for a byte-identical retry.
		auto m = endpoint.make_message(kMaxDecoded);
		const auto expected = fill(m, kMaxDecoded, 0x40);
		transport.refuse = true;
		check(endpoint.send(m) == cobs::SendResult::Failed,
		      "a refused transport start leaves the message with the coordinator");
		check(static_cast<bool>(m) && !m.append_native(uint8_t{0xFF}) && !m.reserve(m.capacity()),
		      "the private Encoded state refuses public building operations");
		transport.refuse = false;
		check(endpoint.send(m) == cobs::SendResult::Sent,
		      "the same message can be retried");
		check(transport.attempts.size() >= 2u &&
		      transport.attempts[transport.attempts.size() - 2u] == transport.attempts.back() &&
		      transport.accepted.back() == cobs_test::frame(expected, Engine::length_size),
		      "and both attempts use one byte-identical canonical frame");
		transport.finish();
		endpoint.poll();
	}
	check(endpoint.storage().tx_available() == kTxBlocks,
	      "every coordinated message released its block");
	check(endpoint.storage().tx_stats().rejected == 0, "and none was released twice");
}

// The frames a grown message produces must be indistinguishable from the ones
// a pre-sized message produces: growth is an allocation strategy, not a wire
// format.
void testCoordinatorEncodingAcrossGrowthHistories()
{
	using Engine = cobs::Endpoint<SpyStorage>;
	Engine endpoint;
	CaptureTransport transport;
	check(bindTransport(endpoint, transport), "the coordinator binds for growth histories");
	const std::vector<std::size_t> lengths{0, 1, 2, 253, 254, 255, 509, 1024};

	bool all_ok = true;
	for (const std::size_t n : lengths) {
		std::vector<uint8_t> body(n);
		for (std::size_t i = 0; i < n; ++i) {
			body[i] = static_cast<uint8_t>((i % 5 == 2) ? 0x00 : (0x41 + (i % 60)));
		}

		{	// Grown one byte at a time: many reallocations.
			auto m = endpoint.make_message(0);
			bool ok = true;
			for (const uint8_t b : body) { ok = ok && m.append_native(b); }
			all_ok = all_ok && ok && sendsAs(endpoint, transport, m, body);
		}
		{	// Grown once, from a hint just short of the length.
			auto m = endpoint.make_message(n / 2);
			all_ok = all_ok && m.append_bytes(std::span<const uint8_t>{body}) &&
			         sendsAs(endpoint, transport, m, body);
		}
		{	// Never grown: reserved up front.
			auto m = endpoint.make_message(n);
			all_ok = all_ok && m.append_bytes(std::span<const uint8_t>{body}) &&
			         sendsAs(endpoint, transport, m, body);
		}
	}
	check(all_ok, "frames are identical whether the message grew once, "
	              "many times, or never (" + std::to_string(lengths.size()) +
	              " lengths x 3 histories)");
	check(endpoint.storage().allocations == endpoint.storage().frees,
	      "and every block from every history came back");
}


/* ======================= what the wire will not carry =================== */

/*
 * The constraint is the point: a comment about padding protects only the
 * people who read comments. These are compile-time assertions, so a
 * regression here is a build failure rather than three bytes of this
 * compiler's padding arriving at somebody else's parser.
 */
struct PaddedStruct {
	uint8_t  a;
	uint32_t b;
};

enum class WireOp : uint16_t { Ping = 1, Pong = 2 }; // the spelling a wire wants
enum class BoolBackedEnum : bool { No, Yes };        // legal C++, not a wire type

template<class M, class T>
concept CanAppendNative = requires(M& m, const T& v) { m.append_native(v); };

template<class M, class T>
concept CanAppendSpan = requires(M& m, std::span<const T> s) { m.append_native(s); };

template<class M>
concept HasPublicEncode = requires(M& m) { m.encode(); };

template<class M>
concept HasPublicEncodedState = requires(const M& m) { m.encoded(); };

template<class M, class Storage>
concept HasPublicStorageIdentity = requires(const M& m, const Storage& storage) {
	m.belongs_to(storage);
};

template<class M>
concept HasPublicSurrender = requires(M& m) { m.surrender_block(); };

void testCoordinatorOnlyOperations()
{
	static_assert(!HasPublicEncode<Message>);
	static_assert(!HasPublicEncodedState<Message>);
	static_assert(!HasPublicStorageIdentity<Message, TxPool>);
	static_assert(!HasPublicSurrender<Message>);
	check(true,
	      "encode, encoded state, storage identity and block surrender are coordinator-only");
}

void testTypeConstraints()
{
	using M = cobs::Message<TxPool>;

	// Accepted: the types that mean something on a wire.
	static_assert(CanAppendNative<M, uint8_t>);
	static_assert(CanAppendNative<M, int8_t>);
	static_assert(CanAppendNative<M, uint16_t>);
	static_assert(CanAppendNative<M, int32_t>);
	static_assert(CanAppendNative<M, uint64_t>);
	static_assert(CanAppendNative<M, float>);
	static_assert(CanAppendNative<M, double>);
	static_assert(CanAppendNative<M, char>);
	static_assert(CanAppendNative<M, Op>);          // enum
	static_assert(CanAppendNative<M, std::byte>);
	check(true, "scalars, enums and std::byte are accepted");

	// Refused, each for its own reason.
	static_assert(!CanAppendNative<M, PaddedStruct>,   // padding + field order + ABI
	              "a struct must not be silently blitted onto the wire");
	static_assert(!CanAppendNative<M, bool>,           // true is any non-zero pattern
	              "bool has no fixed object representation");
	static_assert(!CanAppendNative<M, const char*>,    // meaningless to the receiver
	              "a pointer value must not be sendable");
	static_assert(!CanAppendNative<M, uint32_t PaddedStruct::*>,
	              "a member pointer must not be sendable");
	static_assert(CanAppendNative<M, std::span<const uint8_t>>,
	              "a native-scalar span selects the span overload");
	check(true, "structs, bool, pointers and member pointers are refused");

	/* volatile deserves its own assertion, because the constraint alone did
	 * NOT catch it. Before the concept excluded volatile explicitly,
	 * cobs::detail::NativeScalar<volatile uint32_t> was true and this very
	 * CanAppendNative reported
	 * the type as writable — a requires-expression checks that a call is
	 * viable, not that its body instantiates, and the body failed on
	 * `const volatile void*` -> `const void*`. The assertion below is the one
	 * that would have failed then. */
	static_assert(!cobs::detail::NativeScalar<volatile uint32_t>,
	              "volatile must not satisfy the concept");
	static_assert(!CanAppendNative<M, volatile uint32_t>,
	              "and a volatile value must not be writable");
	static_assert(!CanAppendNative<M, volatile Op>);
	static_assert(!CanAppendSpan<M, volatile uint32_t>,
	              "nor an array of them");
	static_assert(CanAppendNative<M, const uint32_t>,
	              "while plain const is still perfectly writable");
	check(true, "volatile is refused, so an MMIO read has to be written out loud");

	/* An enumeration may name bool as its underlying type, which sailed
	 * through the enum branch and put a bool on the wire after all — the one
	 * wire-format hazard this layer enforces rather than documents. The rest
	 * of them (unsized enums under -fshort-enums, size_t, long double) are a
	 * documented rule, since enforcing them would mean banning `int`. */
	static_assert(CanAppendNative<M, WireOp>, "an enum with an explicit width is fine");
	static_assert(!CanAppendNative<M, BoolBackedEnum>,
	              "but an enum backed by bool must not smuggle one through");
	static_assert(!CanAppendSpan<M, BoolBackedEnum>);
	check(true, "an enum whose underlying type is bool is refused too");

	// append_native shares the SAME contract, deliberately — one rule to
	// explain, not two nearly identical ones.
	static_assert(CanAppendSpan<M, uint16_t>);
	static_assert(CanAppendSpan<M, int64_t>);
	static_assert(CanAppendSpan<M, Op>);
	static_assert(CanAppendSpan<M, std::byte>);
	static_assert(!CanAppendSpan<M, PaddedStruct>);
	static_assert(!CanAppendSpan<M, bool>);
	static_assert(!CanAppendSpan<M, const char*>);
	check(true, "and append_native accepts and refuses exactly the same types");
}

/* ===================== the default reserve pays off ===================== */

// The measured reason Endpoint::default_capacity_hint is not zero. Same policy rule
// as the heap one, so these counts are the heap's counts.
void testDefaultHintAvoidsTheLadder()
{
	const auto buildByteAtATime = [](SpyStorage& spy, const std::size_t hint,
	                                 const std::size_t payload) {
		cobs::Message<SpyStorage> m{spy, hint};
		for (std::size_t i = 0; i < payload; ++i) {
			if (!m.append_native(static_cast<uint8_t>(i))) { return false; }
		}
		return m.size() == payload;
	};

	{	// From zero: the whole geometric ladder, which is what the default
		// exists to avoid.
		SpyStorage spy;
		check(buildByteAtATime(spy, 0, 100), "100 bytes appended from a zero hint");
		check(spy.allocations == 14,
		      "costing 14 allocations — the ladder 0,1,2,3,4,6,9,13,19,...");
	}
	{	// From 32, which is what make_message() reserves.
		SpyStorage spy;
		check(buildByteAtATime(spy, 32, 100), "the same 100 bytes from a hint of 32");
		check(spy.allocations == 4, "costing four: 32 -> 48 -> 72 -> 108");
	}
	{	// A short frame, the common case, costs exactly one.
		SpyStorage spy;
		check(buildByteAtATime(spy, 32, 12), "a twelve-byte frame from a hint of 32");
		// One allocation, and the one release that ends its life: no
		// reallocation happened at any point.
		check(spy.allocations == 1 && spy.frees == 1,
		      "costing exactly one allocation, with no reallocation at all");
	}
}


/* ====================== the hidden length prefix ======================== */

// A wide-format policy (limits above 255) so both header widths are exercised
// by the same test bodies. SpyStorage is already wide; this one is narrow and
// heap-exact, for contrast.
class NarrowHeap final {
public:
	using Format = cobs::Format<255>;
	using RxBlock = cobs::RxBlock<NarrowHeap>;

	[[nodiscard]] RxBlock* acquire_rx(std::size_t) noexcept { return nullptr; }
	void release_rx(RxBlock*) noexcept {}

	[[nodiscard]] cobs::TxBlock acquire_tx(const std::size_t requested) noexcept
	{
		if (requested > Format::max_send_size) { return {}; }
		void* const memory =
			::operator new(Format::tx_storage_size_for_capacity(requested), std::nothrow);
		if (memory == nullptr) { return {}; }
		return {static_cast<std::byte*>(memory), requested};
	}
	void release_tx(const cobs::TxBlock block) noexcept
	{
		::operator delete(static_cast<void*>(block.memory));
	}
};

void testLengthPrefixIsHiddenAndCorrect()
{
	static_assert(cobs::Message<NarrowHeap>::length_size == 1, "255/255 fits one byte");
	static_assert(cobs::Message<SpyStorage>::length_size == 2, "4096 needs two");
	check(true, "the two header widths are both under test");

	{	// The prefix is invisible to the container API.
		using Engine = cobs::Endpoint<NarrowHeap>;
		Engine endpoint;
		CaptureTransport transport;
		check(bindTransport(endpoint, transport), "the one-byte-format coordinator binds");
		auto m = endpoint.make_message(16);
		check(m.size() == 0 && m.capacity() == 16,
		      "size() and capacity() count application bytes only");
		const auto body = std::vector<uint8_t>{0x11, 0x22, 0x33};
		check(m.append_bytes(std::span<const uint8_t>{body}), "three bytes are appended");
		check(m.size() == 3, "and the header is not one of them");
		check(sendsAs(endpoint, transport, m, body), "while the wire carries [length][body]");
	}
	{	// Same for the two-byte format, including a length above 255 that a
		// one-byte header could not express.
		using Engine = cobs::Endpoint<SpyStorage>;
		Engine endpoint;
		CaptureTransport transport;
		check(bindTransport(endpoint, transport), "the two-byte-format coordinator binds");
		auto m = endpoint.make_message(0);
		const auto body = [] {
			std::vector<uint8_t> v(300);
			for (std::size_t i = 0; i < v.size(); ++i) {
				v[i] = static_cast<uint8_t>((i % 11 == 4) ? 0 : (0x20 + (i % 200)));
			}
			return v;
		}();
		check(m.append_bytes(std::span<const uint8_t>{body}), "a 300-byte payload is appended");
		check(m.size() == 300, "counted in application bytes");
		check(sendsAs(endpoint, transport, m, body),
		      "and framed with a two-byte little-endian length");
	}
}

void testMaximumFormatFrame()
{
	using Engine = cobs::Endpoint<cobs::Heap<cobs::Format<65535>>>;
	Engine endpoint;
	CaptureTransport transport;
	check(bindTransport(endpoint, transport), "the maximum-format coordinator binds");

	std::vector<uint8_t> body(Engine::max_send_size);
	for (std::size_t i = 0; i < body.size(); ++i) {
		body[i] = static_cast<uint8_t>((i % 251u == 7u) ? 0u : 1u + i % 254u);
	}
	auto message = endpoint.make_message(body.size());
	check(message.append_bytes(std::span<const uint8_t>{body}) &&
	      message.size() == Engine::max_send_size,
	      "all 65535 payload bytes fit the widest supported format");
	check(sendsAs(endpoint, transport, message, body),
	      "and encode in place as one canonical maximum engine frame");
}

/*
 * The header shifts every COBS block boundary by H bytes, so the interesting
 * payload lengths are the ones that put H + S on a boundary rather than S.
 * Getting this wrong produces a frame that is correct for every length except
 * a handful — the classic way an encoder passes its tests and fails in the
 * field.
 */
void testHeaderShiftsTheCobsBoundaries()
{
	const auto run = [](auto& endpoint, CaptureTransport& transport,
	                    const char* name, const std::size_t H) {
		using Engine = std::decay_t<decltype(endpoint)>;
		using M = typename Engine::Message;
		bool ok = true;
		std::size_t cases = 0;
		for (const std::size_t decoded : {std::size_t{253}, std::size_t{254},
		                                  std::size_t{255}, std::size_t{508},
		                                  std::size_t{509}, std::size_t{510}}) {
			if (decoded < H) { continue; }
			const std::size_t S = decoded - H;
			if (S > M::max_payload_size) { continue; }
			// Zero-free is the worst case for COBS expansion.
			std::vector<uint8_t> body(S);
			for (std::size_t i = 0; i < S; ++i) {
				body[i] = static_cast<uint8_t>(1 + (i % 255));
			}
			auto m = endpoint.make_message(0);
			ok = ok && m.append_bytes(std::span<const uint8_t>{body}) &&
			     sendsAs(endpoint, transport, m, body);
			++cases;

			// And once with zeros, which move the block boundaries around.
			std::vector<uint8_t> zeros(S);
			for (std::size_t i = 0; i < S; ++i) {
				zeros[i] = static_cast<uint8_t>((i % 3 == 0) ? 0 : (1 + (i % 250)));
			}
			auto m2 = endpoint.make_message(0);
			ok = ok && m2.append_bytes(std::span<const uint8_t>{zeros}) &&
			     sendsAs(endpoint, transport, m2, zeros);
			++cases;
		}
		check(ok, std::string(name) + ": " + std::to_string(cases) +
		          " payloads placing header+body on a COBS block boundary");
	};

	cobs::Endpoint<SpyStorage> wide;
	CaptureTransport wide_transport;
	check(bindTransport(wide, wide_transport), "the wide boundary coordinator binds");
	run(wide, wide_transport, "two-byte header", 2);

	cobs::Endpoint<NarrowHeap> narrow;
	CaptureTransport narrow_transport;
	check(bindTransport(narrow, narrow_transport), "the narrow boundary coordinator binds");
	run(narrow, narrow_transport, "one-byte header", 1);
}

} // namespace

int main()
{
	group("CoordinatorBoundary");
	testCoordinatorOnlyOperations();

	group("Ownership");
	testAcquireAndRelease();
	testExhaustionAndConcurrentMessages();
	testMoveSemantics();

	group("Container");
	testCreation();
	testGrowthSequence();
	testLargeJumpIsOneAllocation();
	testDefaultHintAvoidsTheLadder();
	testIntermediateOverallocation();
	testNoGrowthWhenItFits();
	testFailedGrowthChangesNothing();
	testReserve();

	group("Serializers");
	testTypeConstraints();
	testSerializers();
	testSerializerFailuresLeaveTheMessageUsable();
	testOversizeIsRefusedNotClamped();

	group("Format");
	testLengthPrefixIsHiddenAndCorrect();
	testMaximumFormatFrame();
	testHeaderShiftsTheCobsBoundaries();

	group("CoordinatorEncoding");
	testCoordinatorEncoding();
	testCoordinatorEncodingAcrossGrowthHistories();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
