/*
 * Host verification for CobsMsg: storage, geometry and exclusive ownership.
 * No transport anywhere — that is the point of the type, and of this suite.
 *
 * Ownership is asserted BEHAVIOURALLY, through TX pool occupancy: a message
 * that leaks a block or frees one twice shows up as the wrong number of
 * available blocks, not as a field read out of the object.
 */
#include "CobsMsg.h"
#include "detail/StaticBlockPool.h"
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

using TxPool = cobs::detail::StaticBlockPool<cobs_max_wire_size(kMaxDecoded), kTxBlocks, 1>;
using Msg = CobsMsg<kMaxDecoded, TxPool>;

std::vector<uint8_t> fill(Msg& m, const uint8_t tag, const std::size_t n)
{
	std::vector<uint8_t> expected(n);
	const auto payload = m.reserve(n);
	for (std::size_t i = 0; i < n; ++i) {
		expected[i] = static_cast<uint8_t>(tag + i);
		payload[i] = expected[i];
	}
	return expected;
}

/* ============================== ownership =============================== */

void testAcquireAndRelease()
{
	TxPool pool;
	check(pool.available() == kTxBlocks, "the TX pool starts full");
	{
		Msg m(pool);
		check(static_cast<bool>(m), "a message acquires a block");
		check(pool.available() == kTxBlocks - 1, "which the pool records as in use");
		check(m.payload_size() == 0, "and starts with no payload reserved");
	}
	check(pool.available() == kTxBlocks, "destroying a Building message returns the block");

	{	// The same, but encoded first — a different state, same obligation.
		Msg m(pool);
		(void)fill(m, 0x10, 8);
		check(!m.encode().empty(), "the message encodes");
		check(m.encoded(), "and is now Encoded");
		check(pool.available() == kTxBlocks - 1, "still holding its block");
	}
	check(pool.available() == kTxBlocks, "destroying an Encoded message returns it too");

	{	// A default-constructed message owns nothing and must not free anything.
		Msg empty;
		check(!empty, "a default-constructed message is empty");
		check(empty.reserve(4).empty(), "and refuses to reserve");
		check(empty.encode().empty(), "and has nothing to encode");
	}
	check(pool.available() == kTxBlocks && pool.stats().rejected == 0,
	      "an empty message frees nothing on destruction");
}

void testExhaustionAndConcurrentMessages()
{
	TxPool pool;
	Msg a(pool);
	Msg b(pool);
	check(static_cast<bool>(a) && static_cast<bool>(b),
	      "several messages can be held at once");
	check(pool.available() == 0, "consuming the whole TX pool");

	Msg c(pool);
	check(!c, "a further message from a dry pool is empty rather than a failure code");
	check(c.reserve(4).empty(), "and stays unusable");

	{	// Distinct blocks, or two messages would encode over each other.
		const auto pa = a.reserve(4);
		const auto pb = b.reserve(4);
		check(pa.data() != pb.data(), "concurrent messages own distinct blocks");
	}

	a = Msg{};
	check(pool.available() == 1, "assigning an empty message over one releases its block");
	Msg d(pool);
	check(static_cast<bool>(d), "and the pool hands that block out again");
	check(pool.stats().rejected == 0, "with no block freed twice");
}

void testMoveSemantics()
{
	TxPool pool;
	{
		Msg a(pool);
		const auto expected = fill(a, 0x20, 6);

		Msg b = std::move(a);
		check(!a, "a moved-from message is empty");
		check(static_cast<bool>(b) && b.payload_size() == 6,
		      "and the destination carries the payload");
		check(pool.available() == kTxBlocks - 1,
		      "the block moved rather than being duplicated or lost");

		const auto wire = b.encode();
		check(std::vector<uint8_t>(wire.begin(), wire.end()) == cobs_test::encode(expected),
		      "the moved message still encodes the bytes that were written");
	}
	check(pool.available() == kTxBlocks, "and is released once");

	{	// Move assignment must release what it overwrites, not leak it.
		Msg a(pool);
		Msg b(pool);
		check(pool.available() == 0, "two blocks held");
		b = std::move(a);
		check(pool.available() == 1, "move assignment released b's own block first");
		check(!a && static_cast<bool>(b), "and transferred a's");

		// Self-move must not destroy the message.
		Msg& alias = b;
		b = std::move(alias);
		check(static_cast<bool>(b), "self move-assignment leaves the message intact");
	}
	check(pool.available() == kTxBlocks, "everything is back");
	check(pool.stats().rejected == 0, "with nothing double-freed");

	// Exclusive ownership is a compile-time property, not a convention.
	static_assert(!std::is_copy_constructible_v<Msg>, "CobsMsg must not be copyable");
	static_assert(!std::is_copy_assignable_v<Msg>, "CobsMsg must not be copy-assignable");
	check(true, "copying is rejected at compile time");
}

/* =============================== geometry =============================== */

void testPayloadGeometry()
{
	TxPool pool;
	Msg m(pool);

	const auto payload = m.reserve(kMaxDecoded);
	check(payload.size() == kMaxDecoded, "the whole protocol limit can be reserved");
	check(m.reserve(kMaxDecoded + 1).empty(), "one byte more is refused");
	check(m.payload_size() == kMaxDecoded, "and the refusal leaves the reservation alone");

	// The payload must sit exactly raw_offset into the block, so that the
	// encoder has precisely the headroom the overlap proof requires.
	const auto probe = m.reserve(1);
	const auto* const block_start = probe.data() - Msg::raw_offset;
	check(probe.data() == block_start + Msg::raw_offset,
	      "the payload begins exactly raw_offset into the block");
	check(Msg::raw_offset == cobs_raw_offset(kMaxDecoded) &&
	      Msg::wire_capacity == cobs_max_wire_size(kMaxDecoded),
	      "with the geometry the encoder's proof assumes");

	check(m.reserve(0).empty() && m.payload_size() == 0,
	      "reserving zero bytes is legal and yields an empty payload");
}

/* ================================ encode ================================ */

void testEncoding()
{
	TxPool pool;

	// Every interesting length, cross-checked against the reference encoder.
	for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{2},
	                            kMaxDecoded - 1, kMaxDecoded}) {
		Msg m(pool);
		const auto expected_payload = fill(m, 0x30, n);
		const auto wire = m.encode();
		const std::vector<uint8_t> got(wire.begin(), wire.end());
		check(got == cobs_test::encode(expected_payload),
		      "payload of " + std::to_string(n) + " bytes encodes canonically in place");
		check(wire.size() <= Msg::wire_capacity, "and fits the block it was written into");
	}

	{	// Encoding is idempotent, which is what makes a failed send retryable.
		Msg m(pool);
		(void)fill(m, 0x40, 5);
		const auto first = m.encode();
		const std::vector<uint8_t> copy(first.begin(), first.end());
		const auto again = m.encode();
		check(std::vector<uint8_t>(again.begin(), again.end()) == copy,
		      "encoding twice yields the identical frame");
		check(again.data() == first.data(), "from the same block, with no re-encoding");
	}

	{	// Once encoded the raw payload is gone, so it must not be writable.
		Msg m(pool);
		(void)fill(m, 0x50, 4);
		(void)m.encode();
		check(m.reserve(4).empty(), "an Encoded message refuses to reserve again");
		check(m.encoded(), "and stays Encoded");
	}
	check(pool.available() == kTxBlocks, "every message released its block");
	check(pool.stats().rejected == 0, "and none was released twice");
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

	group("Encode");
	testEncoding();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
