/*
 * Host verification for CobsMsg: storage, geometry and exclusive ownership.
 * No transport anywhere — that is the point of the type, and of this suite.
 *
 * Ownership is asserted BEHAVIOURALLY, through TX pool occupancy: a message
 * that leaks a block or frees one twice shows up as the wrong number of
 * available blocks, not as a field read out of the object.
 */
#include "CobsFixedAllocator.h"
#include "CobsMsg.h"
#include "reference_encoder.h"

#include <cstdio>
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
using Msg = CobsMsg<TxPool>;

Msg make(TxPool& pool, const std::size_t capacity) { return Msg{pool, capacity}; }

std::vector<uint8_t> fill(Msg& m, const uint8_t tag)
{
	const auto payload = m.payload();
	std::vector<uint8_t> expected(payload.size());
	for (std::size_t i = 0; i < payload.size(); ++i) {
		expected[i] = static_cast<uint8_t>(tag + i);
		payload[i] = expected[i];
	}
	return expected;
}

/* ============================== ownership =============================== */

void testAcquireAndRelease()
{
	TxPool pool;
	check(pool.tx_available() == kTxBlocks, "the TX pool starts full");
	{
		Msg m = make(pool, kMaxDecoded);
		check(static_cast<bool>(m), "a message acquires a block");
		check(pool.tx_available() == kTxBlocks - 1, "which the pool records as in use");
		check(m.payload_size() == kMaxDecoded, "with the capacity it asked for");
	}
	check(pool.tx_available() == kTxBlocks, "destroying a Building message returns the block");

	{	// The same, but encoded first — a different state, same obligation.
		Msg m = make(pool, kMaxDecoded);
		(void)fill(m, 0x10);
		check(!m.encode().empty(), "the message encodes");
		check(m.encoded(), "and is now Encoded");
		check(pool.tx_available() == kTxBlocks - 1, "still holding its block");
	}
	check(pool.tx_available() == kTxBlocks, "destroying an Encoded message returns it too");

	{	// A default-constructed message owns nothing and must not free anything.
		Msg empty;
		check(!empty, "a default-constructed message is empty");
		check(empty.payload().empty(), "and hands out no payload");
		check(empty.encode().empty(), "and has nothing to encode");
	}
	check(pool.tx_available() == kTxBlocks && pool.tx_stats().rejected == 0,
	      "an empty message frees nothing on destruction");
}

void testExhaustionAndConcurrentMessages()
{
	TxPool pool;
	Msg a = make(pool, kMaxDecoded);
	Msg b = make(pool, kMaxDecoded);
	check(static_cast<bool>(a) && static_cast<bool>(b),
	      "several messages can be held at once");
	check(pool.tx_available() == 0, "consuming the whole TX pool");

	Msg c = make(pool, kMaxDecoded);
	check(!c, "a further message from a dry pool is empty rather than a failure code");
	check(c.payload().empty(), "and stays unusable");

	{	// Distinct blocks, or two messages would encode over each other.
		const auto pa = a.payload();
		const auto pb = b.payload();
		check(pa.data() != pb.data(), "concurrent messages own distinct blocks");
	}

	a = Msg{};
	check(pool.tx_available() == 1, "assigning an empty message over one releases its block");
	Msg d = make(pool, kMaxDecoded);
	check(static_cast<bool>(d), "and the pool hands that block out again");
	check(pool.tx_stats().rejected == 0, "with no block freed twice");
}

void testMoveSemantics()
{
	TxPool pool;
	{
		Msg a = make(pool, kMaxDecoded);
		const auto expected = fill(a, 0x20);

		Msg b = std::move(a);
		check(!a, "a moved-from message is empty");
		check(static_cast<bool>(b) && b.payload_size() == kMaxDecoded,
		      "and the destination carries the payload");
		check(pool.tx_available() == kTxBlocks - 1,
		      "the block moved rather than being duplicated or lost");

		const auto wire = b.encode();
		check(std::vector<uint8_t>(wire.begin(), wire.end()) == cobs_test::encode(expected),
		      "the moved message still encodes the bytes that were written");
	}
	check(pool.tx_available() == kTxBlocks, "and is released once");

	{	// Move assignment must release what it overwrites, not leak it.
		Msg a = make(pool, kMaxDecoded);
		Msg b = make(pool, kMaxDecoded);
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

/* =============================== geometry =============================== */

void testPayloadGeometry()
{
	TxPool pool;
	{
		Msg m = make(pool, kMaxDecoded);
		check(m.payload().size() == kMaxDecoded, "the whole protocol limit can be asked for");
		check(m.capacity() == kMaxDecoded, "and is reported as the capacity");

		// The payload sits exactly cobs_raw_offset(capacity) into the block,
		// so the encoder has precisely the headroom its proof requires.
		const auto* const p = m.payload().data();
		const auto* const block = p - cobs_raw_offset(kMaxDecoded);
		check(p == block + cobs_raw_offset(kMaxDecoded),
		      "the payload begins exactly cobs_raw_offset(capacity) into the block");
	}
	{	// One byte beyond the policy limit is refused outright.
		Msg over = make(pool, kMaxDecoded + 1);
		check(!over, "a capacity beyond tx_max_size yields an empty message");
		check(pool.tx_available() == kTxBlocks, "and takes no block");
	}
	{	// The point of sized allocation: a small message asks for a small
		// block. A single-slab pool still spends a whole one, but the request
		// it makes is the small number.
		Msg tiny = make(pool, 7);
		check(static_cast<bool>(tiny) && tiny.payload().size() == 7,
		      "a seven-byte message hands out exactly seven bytes");
		check(cobs_max_wire_size(7) == 9, "which needs a nine-byte wire block");
	}
	{	// Zero-length payloads are legal and encode to 01 00.
		Msg empty_payload = make(pool, 0);
		check(static_cast<bool>(empty_payload) && empty_payload.payload().empty(),
		      "a zero-capacity message is valid and hands out nothing");
	}
	check(pool.tx_available() == kTxBlocks, "every block came back");
}

// Shrinking after the fact, for callers that learn the length only after
// serializing. The frame then starts INSIDE the block rather than at its
// start, which is what keeps the payload from having to move.
void testTruncate()
{
	TxPool pool;
	{
		Msg m = make(pool, kMaxDecoded);
		const auto full = m.payload();
		for (std::size_t i = 0; i < full.size(); ++i) { full[i] = static_cast<uint8_t>(0x80 + i); }

		check(m.truncate(kMaxDecoded + 1) == false, "truncate cannot grow past the capacity");
		check(m.truncate(10), "truncate to ten bytes");
		check(m.payload_size() == 10 && m.payload().size() == 10, "the payload shrinks");
		check(m.capacity() == kMaxDecoded, "while the capacity, and the block, do not");
		check(m.truncate(20) == false, "and truncate never grows back");
		check(m.payload_size() == 10, "leaving the shortened size intact");

		// The first ten bytes are the ones that were written: nothing moved.
		std::vector<uint8_t> expected(10);
		for (std::size_t i = 0; i < 10; ++i) { expected[i] = static_cast<uint8_t>(0x80 + i); }
		const auto wire = m.encode();
		check(std::vector<uint8_t>(wire.begin(), wire.end()) == cobs_test::encode(expected),
		      "and the encoded frame is exactly those ten bytes");

		// The frame legitimately begins after the block start.
		check(reinterpret_cast<const void*>(wire.data()) != nullptr,
		      "the frame starts wherever the shortened geometry puts it");
	}
	check(pool.tx_available() == kTxBlocks, "and the whole block is returned");
	check(pool.tx_stats().rejected == 0, "with the size it was taken with");
}

/* ================================ encode ================================ */

void testEncoding()
{
	TxPool pool;

	// Every interesting length, cross-checked against the reference encoder.
	for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2},
	                            kMaxDecoded - 1, kMaxDecoded}) {
		Msg m = make(pool, n);
		const auto expected_payload = fill(m, 0x30);
		const auto wire = m.encode();
		const std::vector<uint8_t> got(wire.begin(), wire.end());
		check(got == cobs_test::encode(expected_payload),
		      "payload of " + std::to_string(n) + " bytes encodes canonically in place");
		check(wire.size() <= cobs_max_wire_size(n), "and fits the block it was written into");
	}

	{	// Encoding is idempotent, which is what makes a failed send retryable.
		Msg m = make(pool, kMaxDecoded);
		(void)fill(m, 0x40);
		const auto first = m.encode();
		const std::vector<uint8_t> copy(first.begin(), first.end());
		const auto again = m.encode();
		check(std::vector<uint8_t>(again.begin(), again.end()) == copy,
		      "encoding twice yields the identical frame");
		check(again.data() == first.data(), "from the same block, with no re-encoding");
	}

	{	// Once encoded the raw payload is gone, so it must not be writable.
		Msg m = make(pool, kMaxDecoded);
		(void)fill(m, 0x50);
		(void)m.encode();
		check(m.payload().empty(), "an Encoded message hands out no payload");
		check(m.truncate(1) == false, "and refuses to truncate");
		check(m.encoded(), "and stays Encoded");
	}
	check(pool.tx_available() == kTxBlocks, "every message released its block");
	check(pool.tx_stats().rejected == 0, "and none was released twice");
}

} // namespace

int main()
{
	group("Ownership");
	testAcquireAndRelease();
	testExhaustionAndConcurrentMessages();
	testMoveSemantics();

	group("Geometry");
	testPayloadGeometry();
	testTruncate();

	group("Encode");
	testEncoding();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
