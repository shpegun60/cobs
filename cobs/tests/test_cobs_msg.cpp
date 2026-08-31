/*
 * Host verification for CobsMsg: block ownership, container semantics, the
 * growth rule and the serializers. No transport anywhere — that is the point
 * of the type, and of this suite.
 *
 * Two things are asserted only INDIRECTLY, on purpose:
 *
 *   - Ownership, through TX pool occupancy and through a counting policy. A
 *     message that leaks a block or frees one twice shows up as the wrong
 *     number of available blocks, never as a field read out of the object.
 *   - Payload contents, through encode() and the reference encoder. CobsMsg
 *     hands out no writable payload span, so there is nothing to peek at; a
 *     wrong byte, a lost byte or a botched growth copy all surface as a wrong
 *     wire frame.
 */
#include "CobsFixedAllocator.h"
#include "CobsHeapAllocator.h"
#include "CobsMsg.h"
#include "reference_encoder.h"

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
constexpr std::size_t kTxBlocks = 2; // the recommended default: build one while one flies

using TxPool = CobsFixedAllocator<kMaxDecoded, 1, kMaxDecoded, kTxBlocks>;
using HeapPool = CobsHeapAllocator<kMaxDecoded, kMaxDecoded>;
using Msg = CobsMsg<TxPool>;

// A message filled with `n` recognisable bytes, plus the bytes themselves for
// the oracle to check against.
template<class M>
std::vector<uint8_t> fill(M& m, const std::size_t n, const uint8_t tag)
{
	std::vector<uint8_t> expected(n);
	for (std::size_t i = 0; i < n; ++i) {
		expected[i] = static_cast<uint8_t>(tag + i);
	}
	if (!m.write_bytes(std::span<const uint8_t>{expected})) {
		expected.clear(); // the caller's assertion will notice
	}
	return expected;
}

// The only way to read a message back: encode it and decode the frame with the
// independent reference implementation.
template<class M>
bool framesAs(M& m, const std::vector<uint8_t>& expected)
{
	const auto wire = m.encode();
	return std::vector<uint8_t>(wire.begin(), wire.end()) == cobs_test::encode(expected);
}

/* ============================== ownership =============================== */

void testAcquireAndRelease()
{
	TxPool pool;
	check(pool.tx_available() == kTxBlocks, "the TX pool starts full");
	{
		Msg m{pool};
		check(static_cast<bool>(m), "a message acquires a block");
		check(pool.tx_available() == kTxBlocks - 1, "which the pool records as in use");
		check(m.size() == 0, "and starts empty");
	}
	check(pool.tx_available() == kTxBlocks, "destroying a Building message returns the block");

	{	// The same, but encoded first — a different state, same obligation.
		Msg m{pool};
		(void)fill(m, 16, 0x10);
		check(!m.encode().empty(), "the message encodes");
		check(m.encoded(), "and is now Encoded");
		check(pool.tx_available() == kTxBlocks - 1, "still holding its block");
	}
	check(pool.tx_available() == kTxBlocks, "destroying an Encoded message returns it too");

	{	// A default-constructed message owns nothing and must not free anything.
		Msg empty;
		check(!empty, "a default-constructed message is empty");
		check(empty.write<uint8_t>(1) == false, "and accepts no writes");
		check(empty.encode().empty(), "and has nothing to encode");
	}
	check(pool.tx_available() == kTxBlocks && pool.tx_stats().rejected == 0,
	      "an empty message frees nothing on destruction");
}

void testExhaustionAndConcurrentMessages()
{
	TxPool pool;
	Msg a{pool};
	Msg b{pool};
	check(static_cast<bool>(a) && static_cast<bool>(b),
	      "several messages can be held at once");
	check(pool.tx_available() == 0, "consuming the whole TX pool");

	Msg c{pool};
	check(!c, "a further message from a dry pool is empty rather than a failure code");
	check(c.write<uint8_t>(0) == false, "and stays unusable");

	{	// Distinct blocks, or two messages would encode over each other.
		const auto ea = fill(a, 8, 0x10);
		const auto eb = fill(b, 8, 0x90);
		check(framesAs(a, ea) && framesAs(b, eb),
		      "concurrent messages own distinct blocks");
	}

	a = Msg{};
	check(pool.tx_available() == 1, "assigning an empty message over one releases its block");
	Msg d{pool};
	check(static_cast<bool>(d), "and the pool hands that block out again");
	check(pool.tx_stats().rejected == 0, "with no block freed twice");
}

void testMoveSemantics()
{
	TxPool pool;
	{
		Msg a{pool};
		const auto expected = fill(a, 20, 0x20);

		Msg b = std::move(a);
		check(!a, "a moved-from message is empty");
		check(static_cast<bool>(b) && b.size() == 20,
		      "and the destination carries the payload");
		check(pool.tx_available() == kTxBlocks - 1,
		      "the block moved rather than being duplicated or lost");
		check(framesAs(b, expected),
		      "the moved message still encodes the bytes that were written");
	}
	check(pool.tx_available() == kTxBlocks, "and is released once");

	{	// Move assignment must release what it overwrites, not leak it.
		Msg a{pool};
		Msg b{pool};
		check(pool.tx_available() == 0, "two blocks held");
		b = std::move(a);
		check(pool.tx_available() == 1, "move assignment released b's own block first");
		check(!a && static_cast<bool>(b), "and transferred a's");

		// Self-move must not destroy the message.
		Msg& alias = b;
		b = std::move(alias);
		check(static_cast<bool>(b), "self move-assignment leaves the message intact");
	}
	check(pool.tx_available() == kTxBlocks, "everything is back");
	check(pool.tx_stats().rejected == 0, "with nothing double-freed");

	// Exclusive ownership is a compile-time property, not a convention.
	static_assert(!std::is_copy_constructible_v<Msg>, "CobsMsg must not be copyable");
	static_assert(!std::is_copy_assignable_v<Msg>, "CobsMsg must not be copy-assignable");
	check(true, "copying is rejected at compile time");
}

/* ========================== container semantics ========================= */

/*
 * A counting policy: exact capacity like the heap one, but it records every
 * allocation and can be told to refuse the next. Growth is only observable
 * through the allocator, which is exactly right — a container that reallocates
 * is measured by how often it does so, not by its internal pointers.
 */
class SpyPolicy final {
public:
	static constexpr std::size_t rx_max_size = 8;
	static constexpr std::size_t tx_max_size = 4096;

	using Packet = RxPacket<SpyPolicy>;
	[[nodiscard]] Packet* allocate_rx() noexcept { return nullptr; }
	void deallocate_rx(Packet*) noexcept {}

	[[nodiscard]] TxAllocation allocate_tx(const std::size_t requested) noexcept
	{
		if (requested > tx_max_size || refuse_next) {
			return {};
		}
		void* const memory = ::operator new(cobs_max_wire_size(requested), std::nothrow);
		if (memory == nullptr) {
			return {};
		}
		++allocations;
		last_request = requested;
		return {static_cast<std::byte*>(memory), requested};
	}

	void deallocate_tx(std::byte* const memory, const std::size_t capacity) noexcept
	{
		if (memory != nullptr) {
			++frees;
			last_freed_capacity = capacity;
		}
		::operator delete(static_cast<void*>(memory));
	}

	std::size_t allocations = 0;
	std::size_t frees = 0;
	std::size_t last_request = 0;
	std::size_t last_freed_capacity = 0;
	bool refuse_next = false;
};

/*
 * A policy that over-allocates the way a segregated allocator would: neither
 * exact like the heap one nor "the whole limit" like the single-slab pool, but
 * somewhere in between. The two shipped policies only exercise the extremes,
 * and a container that had quietly assumed capacity == requested would pass
 * both of them.
 */
class SizeClassPolicy final {
public:
	static constexpr std::size_t rx_max_size = 8;
	static constexpr std::size_t tx_max_size = 4096;

	using Packet = RxPacket<SizeClassPolicy>;
	[[nodiscard]] Packet* allocate_rx() noexcept { return nullptr; }
	void deallocate_rx(Packet*) noexcept {}

	// Its own rule, obeying nothing but the contract: 2n + 1, capped.
	[[nodiscard]] static constexpr std::size_t class_for(const std::size_t n) noexcept
	{
		const std::size_t twice = n * 2u + 1u;
		return (twice > tx_max_size) ? tx_max_size : twice;
	}

	[[nodiscard]] TxAllocation allocate_tx(const std::size_t requested) noexcept
	{
		if (requested > tx_max_size) {
			return {};
		}
		const std::size_t capacity = class_for(requested);
		void* const memory = ::operator new(cobs_max_wire_size(capacity), std::nothrow);
		if (memory == nullptr) {
			return {};
		}
		++allocations;
		return {static_cast<std::byte*>(memory), capacity};
	}

	void deallocate_tx(std::byte* const memory, const std::size_t capacity) noexcept
	{
		if (memory != nullptr) {
			++frees;
			last_freed_capacity = capacity;
		}
		::operator delete(static_cast<void*>(memory));
	}

	std::size_t allocations = 0;
	std::size_t frees = 0;
	std::size_t last_freed_capacity = 0;
};

// The container must work off the REPORTED capacity, never off what it asked
// for. This is the case between the two extremes the shipped policies cover.
void testIntermediateOverallocation()
{
	SizeClassPolicy pool;
	{
		CobsMsg<SizeClassPolicy> m{pool, 7};
		check(m.capacity() == 15, "a policy may grant more than was requested (7 -> 15)");
		check(m.size() == 0, "without putting anything in the message");

		// Fifteen bytes must fit with no reallocation, even though only seven
		// were ever asked for.
		const std::vector<uint8_t> body(15, 0x2A);
		check(m.write_bytes(std::span<const uint8_t>{body}), "the whole grant is usable");
		check(pool.allocations == 1, "with no reallocation");
		check(framesAs(m, body), "and encodes from the granted geometry");
	}
	check(pool.frees == 1 && pool.last_freed_capacity == 15,
	      "the block goes back with the capacity the policy reported, not the 7 asked for");

	{	// A growth must also ask through the policy's rule, and record what
		// came back rather than what it wanted.
		SizeClassPolicy grow;
		CobsMsg<SizeClassPolicy> m{grow, 4};
		check(m.capacity() == 9, "4 -> 9");
		const std::vector<uint8_t> body(20, 0x3B);
		check(m.write_bytes(std::span<const uint8_t>{body}), "20 bytes need a growth");
		// target = max(20, 9 + 4) = 20; the policy answers 41.
		check(m.capacity() == 41, "the container asked for 20 and was granted 41");
		check(grow.allocations == 2 && grow.frees == 1, "in exactly one reallocation");
		check(grow.last_freed_capacity == 9, "the old block returned with ITS capacity");
		check(framesAs(m, body), "and the payload survived the move");
	}
}

void testCreation()
{
	{	// The hint is a capacity hint, never an initial size.
		HeapPool heap;
		CobsMsg<HeapPool> unhinted{heap};
		check(static_cast<bool>(unhinted), "an unhinted message is valid");
		check(unhinted.size() == 0 && unhinted.capacity() == 0,
		      "heap: no hint means no capacity yet, and never any size");

		CobsMsg<HeapPool> hinted{heap, 40};
		check(hinted.size() == 0, "a hint does not put bytes in the message");
		check(hinted.capacity() == 40, "heap: the hint is honoured exactly");
	}
	{	// The single-slab pool answers with its whole limit either way, so a
		// message on it is born with all the room it will ever need.
		TxPool pool;
		Msg unhinted{pool};
		check(unhinted.size() == 0 && unhinted.capacity() == kMaxDecoded,
		      "fixed: even an unhinted message gets the whole slab");
		Msg hinted{pool, 8};
		check(hinted.capacity() == kMaxDecoded, "fixed: and a small hint changes nothing");
	}
	{	// A hint past the protocol limit is refused rather than clamped: the
		// caller asked for something this instance cannot carry.
		HeapPool heap;
		CobsMsg<HeapPool> over{heap, kMaxDecoded + 1};
		check(!over, "a hint beyond tx_max_size yields an empty message");
	}
}

void testGrowthSequence()
{
	SpyPolicy spy;
	CobsMsg<SpyPolicy> m{spy};
	check(m.capacity() == 0 && spy.allocations == 1,
	      "the initial block is one allocation, of no capacity");

	// One byte at a time: capacity must follow the documented 1.5x rule, and
	// must never fail to advance.
	std::vector<std::size_t> caps;
	bool writes_ok = true;
	for (std::size_t i = 0; i < 10; ++i) {
		writes_ok = writes_ok && m.write<uint8_t>(static_cast<uint8_t>(i));
		caps.push_back(m.capacity());
	}
	check(writes_ok, "ten single-byte appends all succeed");
	const std::vector<std::size_t> expected{1, 2, 3, 4, 6, 6, 9, 9, 9, 13};
	check(caps == expected, "capacity follows 0 -> 1 -> 2 -> 3 -> 4 -> 6 -> 9 -> 13");
	check(m.size() == 10, "and the size is the number of bytes actually written");

	// Every growth is one allocation and one release; nothing accumulates.
	check(spy.frees == spy.allocations - 1,
	      "each growth released exactly the block it replaced");
	check(spy.last_freed_capacity == 9,
	      "and released it with ITS capacity, not the new one");

	std::vector<uint8_t> written(10);
	for (std::size_t i = 0; i < 10; ++i) { written[i] = static_cast<uint8_t>(i); }
	check(framesAs(m, written),
	      "and every byte survived the reallocations, in order");
}

void testLargeJumpIsOneAllocation()
{
	SpyPolicy spy;
	CobsMsg<SpyPolicy> m{spy, 64};
	check(m.capacity() == 64 && spy.allocations == 1, "starting from a 64-byte hint");

	const std::vector<uint8_t> body(500, 0x5A);
	check(m.write_bytes(std::span<const uint8_t>{body}), "a 500-byte append succeeds");
	check(m.capacity() == 500,
	      "asking for exactly what is required, not walking 96 -> 144 -> 216");
	check(spy.allocations == 2 && spy.last_request == 500,
	      "in a single allocation");
	check(framesAs(m, body), "and the bytes are all there");
}

void testNoGrowthWhenItFits()
{
	SpyPolicy spy;
	CobsMsg<SpyPolicy> m{spy, 256};
	const std::size_t after_create = spy.allocations;

	bool ok = true;
	for (int i = 0; i < 64; ++i) { ok = ok && m.write<uint32_t>(0x11223344u); }
	check(ok, "256 bytes written into a 256-byte capacity");
	check(m.size() == 256 && m.capacity() == 256, "filling it exactly");
	check(spy.allocations == after_create && spy.frees == 0,
	      "with no reallocation and no release at all");

	// And the fixed policy gets this for free, without any hint.
	TxPool pool;
	Msg fixed{pool};
	bool fill_ok = true;
	for (std::size_t i = 0; i < kMaxDecoded; ++i) {
		fill_ok = fill_ok && fixed.write<uint8_t>(static_cast<uint8_t>(i));
	}
	check(fill_ok && fixed.size() == kMaxDecoded,
	      "fixed: a whole tx_max_size payload written one byte at a time");
	check(pool.tx_available() == kTxBlocks - 1,
	      "still on the one block it started with — no growth ever happened");
	check(fixed.write<uint8_t>(0) == false, "and one byte past the limit is refused");
	check(fixed.size() == kMaxDecoded, "leaving the message exactly as it was");
}

void testFailedGrowthChangesNothing()
{
	SpyPolicy spy;
	CobsMsg<SpyPolicy> m{spy, 8};
	const auto written = fill(m, 8, 0x70);
	check(m.size() == 8 && m.capacity() == 8, "eight bytes in an eight-byte capacity");

	const std::size_t allocations = spy.allocations;
	const std::size_t frees = spy.frees;
	spy.refuse_next = true;

	check(m.write<uint32_t>(0xDEADBEEFu) == false, "a write needing growth fails");
	check(m.size() == 8 && m.capacity() == 8, "leaving size and capacity untouched");
	check(spy.frees == frees, "and releasing nothing — the old block is still ours");
	check(spy.allocations == allocations, "with no allocation having succeeded");

	check(m.reserve(64) == false, "reserve fails the same way");
	check(m.size() == 8 && m.capacity() == 8, "and changes nothing either");

	spy.refuse_next = false;
	check(framesAs(m, written),
	      "the payload is byte-for-byte what it was before the failures");
}

void testReserve()
{
	SpyPolicy spy;
	CobsMsg<SpyPolicy> m{spy};
	check(m.reserve(0), "reserving nothing on an empty message succeeds");
	check(spy.allocations == 1, "without allocating");

	check(m.reserve(100), "reserve grows to the exact request");
	check(m.capacity() == 100, "not to a geometric target above it");
	check(spy.allocations == 2, "in one allocation");

	check(m.reserve(50), "a smaller reserve is a no-op");
	check(m.capacity() == 100 && spy.allocations == 2, "and does not shrink or reallocate");

	check(m.reserve(SpyPolicy::tx_max_size + 1) == false,
	      "reserving past tx_max_size is refused");
	check(m.capacity() == 100, "leaving the capacity alone");

	CobsMsg<SpyPolicy> encoded{spy, 4};
	(void)encoded.write<uint32_t>(0);
	(void)encoded.encode();
	check(encoded.reserve(64) == false, "an Encoded message refuses to reserve");
	check(encoded.write<uint8_t>(0) == false, "and refuses to write");
}

/* ============================== serializers ============================= */

enum class Op : uint16_t { Ping = 0x0102, Pong = 0x0304 };

void testSerializers()
{
	SpyPolicy spy;
	CobsMsg<SpyPolicy> m{spy};

	// Everything is appended in target byte order, so the oracle is built the
	// same way — through memcpy of the same objects, never by hand-guessing
	// an endianness.
	std::vector<uint8_t> expected;
	const auto expect = [&expected](const void* p, const std::size_t n) {
		const auto* const bytes = static_cast<const uint8_t*>(p);
		expected.insert(expected.end(), bytes, bytes + n);
	};

	bool ok = true;
	const uint8_t  u8  = 0x11;         ok = ok && m.write(u8);  expect(&u8, 1);
	const int8_t   i8  = -2;           ok = ok && m.write(i8);  expect(&i8, 1);
	const uint16_t u16 = 0x1234;       ok = ok && m.write(u16); expect(&u16, 2);
	const int16_t  i16 = -300;         ok = ok && m.write(i16); expect(&i16, 2);
	const uint32_t u32 = 0xDEADBEEFu;  ok = ok && m.write(u32); expect(&u32, 4);
	const int32_t  i32 = -70000;       ok = ok && m.write(i32); expect(&i32, 4);
	const uint64_t u64 = 0x0102030405060708ull;
	                                   ok = ok && m.write(u64); expect(&u64, 8);
	const int64_t  i64 = -1;           ok = ok && m.write(i64); expect(&i64, 8);
	const float    f   = 1.5f;         ok = ok && m.write(f);   expect(&f, sizeof f);
	const double   d   = -2.25;        ok = ok && m.write(d);   expect(&d, sizeof d);
	const Op       op  = Op::Pong;     ok = ok && m.write(op);  expect(&op, sizeof op);
	check(ok, "every scalar, enum and floating-point value appends");

	{	// A flag goes out as the byte the caller chose, because write<T> does
		// not accept bool at all (see CobsScalar).
		const uint8_t flag = 1;
		ok = m.write(flag);
		expect(&flag, 1);
		const std::byte raw_byte{0xA5};
		ok = ok && m.write(raw_byte);
		expect(&raw_byte, 1);
		check(ok, "and std::byte goes through as itself");
	}
	{	// A byte span, then an array of a wider type.
		const std::vector<uint8_t> blob{0x00, 0xFF, 0x00, 0x7F};
		ok = m.write_bytes(std::span<const uint8_t>{blob});
		expect(blob.data(), blob.size());

		const uint16_t words[] = {0x0001, 0x0200, 0xFFFF};
		ok = ok && m.write_array(std::span<const uint16_t>{words});
		expect(words, sizeof words);
		check(ok, "so do a byte span and an array of a wider type");
	}

	check(m.size() == expected.size(), "the size is the sum of everything written");
	check(framesAs(m, expected),
	      "and the frame is exactly those bytes, canonically encoded (" +
	          std::to_string(expected.size()) + " payload bytes)");
}

void testSerializerFailuresLeaveTheMessageUsable()
{
	SpyPolicy spy;
	CobsMsg<SpyPolicy> m{spy, 4};
	check(m.write<uint32_t>(0x01020304u), "four bytes fit");

	spy.refuse_next = true;
	check(m.write<uint64_t>(0) == false, "an eight-byte write that cannot grow fails");
	const uint8_t blob[16] = {};
	check(m.write_bytes(std::span<const uint8_t>{blob}) == false, "so does a span");
	const uint32_t words[8] = {};
	check(m.write_array(std::span<const uint32_t>{words}) == false, "so does an array");
	check(m.size() == 4, "and none of them moved the size");

	spy.refuse_next = false;
	check(m.write<uint8_t>(0x05), "the message still works once memory is available");
	check(m.size() == 5, "appending after a failed write, not on top of it");

	// The uint32 went out in target order, so build the oracle the same way.
	std::vector<uint8_t> oracle(5);
	const uint32_t v = 0x01020304u;
	std::memcpy(oracle.data(), &v, 4);
	oracle[4] = 0x05;
	check(framesAs(m, oracle), "with the earlier bytes intact");
}

void testOversizeIsRefusedNotClamped()
{
	SpyPolicy spy;
	CobsMsg<SpyPolicy> m{spy};
	const std::vector<uint8_t> huge(SpyPolicy::tx_max_size + 1, 0x33);
	check(m.write_bytes(std::span<const uint8_t>{huge}) == false,
	      "a payload past tx_max_size is refused");
	check(m.size() == 0 && spy.allocations == 1, "with nothing written and nothing allocated");

	const std::vector<uint8_t> exact(SpyPolicy::tx_max_size, 0x44);
	check(m.write_bytes(std::span<const uint8_t>{exact}),
	      "while exactly tx_max_size is accepted");
	check(m.size() == SpyPolicy::tx_max_size && m.capacity() == SpyPolicy::tx_max_size,
	      "filling the message to its limit");
	check(m.write<uint8_t>(0) == false, "after which nothing more fits");
}

/* ================================ encode ================================ */

void testEncoding()
{
	TxPool pool;

	// Every interesting length, cross-checked against the reference encoder.
	for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2},
	                            kMaxDecoded - 1, kMaxDecoded}) {
		Msg m{pool};
		const auto expected = fill(m, n, 0x30);
		check(framesAs(m, expected),
		      "payload of " + std::to_string(n) + " bytes encodes canonically in place");
	}

	{	// Encoding is idempotent, which is what makes a failed send retryable.
		Msg m{pool};
		(void)fill(m, kMaxDecoded, 0x40);
		const auto first = m.encode();
		const std::vector<uint8_t> copy(first.begin(), first.end());
		const auto again = m.encode();
		check(std::vector<uint8_t>(again.begin(), again.end()) == copy,
		      "encoding twice yields the identical frame");
		check(again.data() == first.data(), "from the same block, with no re-encoding");
	}
	check(pool.tx_available() == kTxBlocks, "every message released its block");
	check(pool.tx_stats().rejected == 0, "and none was released twice");
}

// The frames a grown message produces must be indistinguishable from the ones
// a pre-sized message produces: growth is an allocation strategy, not a wire
// format.
void testEncodingAcrossGrowthHistories()
{
	SpyPolicy spy;
	const std::vector<std::size_t> lengths{0, 1, 2, 253, 254, 255, 509, 1024};

	bool all_ok = true;
	for (const std::size_t n : lengths) {
		std::vector<uint8_t> body(n);
		for (std::size_t i = 0; i < n; ++i) {
			body[i] = static_cast<uint8_t>((i % 5 == 2) ? 0x00 : (0x41 + (i % 60)));
		}

		{	// Grown one byte at a time: many reallocations.
			CobsMsg<SpyPolicy> m{spy};
			bool ok = true;
			for (const uint8_t b : body) { ok = ok && m.write(b); }
			all_ok = all_ok && ok && framesAs(m, body);
		}
		{	// Grown once, from a hint just short of the length.
			CobsMsg<SpyPolicy> m{spy, n / 2};
			all_ok = all_ok && m.write_bytes(std::span<const uint8_t>{body}) &&
			         framesAs(m, body);
		}
		{	// Never grown: reserved up front.
			CobsMsg<SpyPolicy> m{spy, n};
			all_ok = all_ok && m.write_bytes(std::span<const uint8_t>{body}) &&
			         framesAs(m, body);
		}
	}
	check(all_ok, "frames are identical whether the message grew once, "
	              "many times, or never (" + std::to_string(lengths.size()) +
	              " lengths x 3 histories)");
	check(spy.allocations == spy.frees,
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
concept CanWrite = requires(M& m, const T& v) { m.write(v); };

template<class M, class T>
concept CanWriteArray = requires(M& m, std::span<const T> s) { m.write_array(s); };

void testTypeConstraints()
{
	using M = CobsMsg<TxPool>;

	// Accepted: the types that mean something on a wire.
	static_assert(CanWrite<M, uint8_t>);
	static_assert(CanWrite<M, int8_t>);
	static_assert(CanWrite<M, uint16_t>);
	static_assert(CanWrite<M, int32_t>);
	static_assert(CanWrite<M, uint64_t>);
	static_assert(CanWrite<M, float>);
	static_assert(CanWrite<M, double>);
	static_assert(CanWrite<M, char>);
	static_assert(CanWrite<M, Op>);          // enum
	static_assert(CanWrite<M, std::byte>);
	check(true, "scalars, enums and std::byte are accepted");

	// Refused, each for its own reason.
	static_assert(!CanWrite<M, PaddedStruct>,   // padding + field order + ABI
	              "a struct must not be silently blitted onto the wire");
	static_assert(!CanWrite<M, bool>,           // true is any non-zero pattern
	              "bool has no fixed object representation");
	static_assert(!CanWrite<M, const char*>,    // meaningless to the receiver
	              "a pointer value must not be sendable");
	static_assert(!CanWrite<M, uint32_t PaddedStruct::*>,
	              "a member pointer must not be sendable");
	static_assert(!CanWrite<M, std::span<const uint8_t>>,
	              "a span is not a scalar; write_bytes takes those");
	check(true, "structs, bool, pointers, member pointers and spans are refused");

	/* volatile deserves its own assertion, because the constraint alone did
	 * NOT catch it. Before the concept excluded volatile explicitly,
	 * CobsScalar<volatile uint32_t> was true and this very CanWrite reported
	 * the type as writable — a requires-expression checks that a call is
	 * viable, not that its body instantiates, and the body failed on
	 * `const volatile void*` -> `const void*`. The assertion below is the one
	 * that would have failed then. */
	static_assert(!CobsScalar<volatile uint32_t>,
	              "volatile must not satisfy the concept");
	static_assert(!CanWrite<M, volatile uint32_t>,
	              "and a volatile value must not be writable");
	static_assert(!CanWrite<M, volatile Op>);
	static_assert(!CanWriteArray<M, volatile uint32_t>,
	              "nor an array of them");
	static_assert(CanWrite<M, const uint32_t>,
	              "while plain const is still perfectly writable");
	check(true, "volatile is refused, so an MMIO read has to be written out loud");

	/* An enumeration may name bool as its underlying type, which sailed
	 * through the enum branch and put a bool on the wire after all — the one
	 * wire-format hazard this layer enforces rather than documents. The rest
	 * of them (unsized enums under -fshort-enums, size_t, long double) are a
	 * documented rule, since enforcing them would mean banning `int`. */
	static_assert(CanWrite<M, WireOp>, "an enum with an explicit width is fine");
	static_assert(!CanWrite<M, BoolBackedEnum>,
	              "but an enum backed by bool must not smuggle one through");
	static_assert(!CanWriteArray<M, BoolBackedEnum>);
	check(true, "an enum whose underlying type is bool is refused too");

	// write_array shares the SAME contract, deliberately — one rule to
	// explain, not two nearly identical ones.
	static_assert(CanWriteArray<M, uint16_t>);
	static_assert(CanWriteArray<M, int64_t>);
	static_assert(CanWriteArray<M, Op>);
	static_assert(CanWriteArray<M, std::byte>);
	static_assert(!CanWriteArray<M, PaddedStruct>);
	static_assert(!CanWriteArray<M, bool>);
	static_assert(!CanWriteArray<M, const char*>);
	check(true, "and write_array accepts and refuses exactly the same types");
}

/* ===================== the default reserve pays off ===================== */

// The measured reason Cobs::default_capacity_hint is not zero. Same policy rule
// as the heap one, so these counts are the heap's counts.
void testDefaultHintAvoidsTheLadder()
{
	const auto buildByteAtATime = [](SpyPolicy& spy, const std::size_t hint,
	                                 const std::size_t payload) {
		CobsMsg<SpyPolicy> m{spy, hint};
		for (std::size_t i = 0; i < payload; ++i) {
			if (!m.write(static_cast<uint8_t>(i))) { return false; }
		}
		return m.size() == payload;
	};

	{	// From zero: the whole geometric ladder, which is what the default
		// exists to avoid.
		SpyPolicy spy;
		check(buildByteAtATime(spy, 0, 100), "100 bytes written from a zero hint");
		check(spy.allocations == 14,
		      "costing 14 allocations — the ladder 0,1,2,3,4,6,9,13,19,...");
	}
	{	// From 32, which is what make_msg() reserves.
		SpyPolicy spy;
		check(buildByteAtATime(spy, 32, 100), "the same 100 bytes from a hint of 32");
		check(spy.allocations == 4, "costing four: 32 -> 48 -> 72 -> 108");
	}
	{	// A short frame, the common case, costs exactly one.
		SpyPolicy spy;
		check(buildByteAtATime(spy, 32, 12), "a twelve-byte frame from a hint of 32");
		// One allocation, and the one release that ends its life: no
		// reallocation happened at any point.
		check(spy.allocations == 1 && spy.frees == 1,
		      "costing exactly one allocation, with no reallocation at all");
	}
}

} // namespace

int main()
{
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

	group("Encode");
	testEncoding();
	testEncodingAcrossGrowthHistories();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
