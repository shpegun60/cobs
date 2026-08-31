/*
 * End-to-end verification of the RX vertical:
 *
 *   encoded bytes -> CobsDecoder -> allocator policy -> RxPacket
 *                 -> intrusive ready queue -> PacketRef -> release -> pool
 *
 * PacketRef's own semantics (copy, move, assignment, self-assignment) are
 * tested here rather than beside the pool, because a reference can now only
 * come from its legitimate owner: adopt() is private to CobsRx, so a test can
 * no longer mint one by hand — which was exactly the hole being closed.
 *
 * Refcounts are therefore checked BEHAVIOURALLY, through pool occupancy. That
 * is a stronger test than reading the field: it asserts the guarantee the
 * application depends on rather than the bookkeeping behind it.
 */
#include "CobsFixedAllocator.h"
#include "CobsRx.h"
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
constexpr std::size_t kBlocks = 4;
using Pool = CobsFixedAllocator<kMaxDecoded, kBlocks, kMaxDecoded, 2>;
using Rx = CobsRx<Pool>;
using Ref = Rx::Ref;

std::vector<uint8_t> payload(const uint8_t tag, const std::size_t n)
{
	std::vector<uint8_t> v(n);
	for (std::size_t i = 0; i < n; ++i) {
		v[i] = static_cast<uint8_t>(tag + i);
	}
	return v;
}

bool matches(const Ref& r, const std::vector<uint8_t>& expected)
{
	if (r.size() != expected.size()) {
		return false;
	}
	const auto d = r.data();
	for (std::size_t i = 0; i < expected.size(); ++i) {
		if (d[i] != expected[i]) {
			return false;
		}
	}
	return true;
}

void feed(Rx& rx, const std::vector<uint8_t>& wire)
{
	rx.consume(std::span<const uint8_t>{wire});
}

/* ============================== end to end ============================== */

void testRoundTripThroughTheWholeStack()
{
	Pool pool;
	{
		Rx rx(pool);
		const auto a = payload(0x10, 5);
		const auto b = payload(0x40, 0);   // the empty packet: 01 00
		const auto c = payload(0x70, kMaxDecoded);

		std::vector<uint8_t> wire;
		for (const auto* p : {&a, &b, &c}) {
			for (const uint8_t x : cobs_test::encode(*p)) { wire.push_back(x); }
		}
		feed(rx, wire);

		check(rx.stats().frames_delivered == 3, "three frames decoded through the stack");
		check(pool.rx_available() == kBlocks - 3, "and each holds a pool block");

		Ref r1 = rx.pop_packet();
		Ref r2 = rx.pop_packet();
		Ref r3 = rx.pop_packet();
		check(matches(r1, a) && matches(r2, b) && matches(r3, c),
		      "packets arrive in order with exactly the bytes that were encoded");
		check(r2.size() == 0, "including the empty packet, which is a real packet");
		check(r3.size() == kMaxDecoded, "and a maximum-size one");
		check(!rx.pop_packet(), "the queue is then empty");
	}
	check(pool.rx_available() == kBlocks, "leaving scope releases every packet");
	check(pool.rx_stats().rejected == 0, "each block was returned exactly once");
}

// A frame split across spans at every possible boundary must still arrive.
void testSpanBoundaries()
{
	const auto p = payload(0x21, 40);
	const auto wire = cobs_test::encode(p);

	bool all_ok = true;
	for (std::size_t cut = 0; cut <= wire.size(); ++cut) {
		Pool pool;
		Rx rx(pool);
		rx.consume(std::span<const uint8_t>{wire.data(), cut});
		rx.consume(std::span<const uint8_t>{wire.data() + cut, wire.size() - cut});
		const Ref r = rx.pop_packet();
		all_ok = all_ok && matches(r, p);
	}
	check(all_ok, "a frame survives a span split at every one of " +
	              std::to_string(wire.size() + 1) + " positions");
}

// The default policy: naming nothing at all must give a working receiver.
void testDefaultAllocator()
{
	using DefaultRx = CobsRx<>;
	static_assert(std::is_same_v<DefaultRx::AllocatorType, CobsHeapAllocator<>>,
	              "the default policy is the heap one");
	static_assert(DefaultRx::max_decoded_size == 1024, "with its default limit");

	DefaultRx::AllocatorType allocator;
	DefaultRx rx(allocator);

	const auto p = payload(0x11, 300);
	rx.consume(std::span<const uint8_t>{cobs_test::encode(p)});
	const DefaultRx::Ref r = rx.pop_packet();
	check(r.size() == p.size(), "CobsRx<> receives a frame with no policy named");
}

/* ========================= the protocol limit =========================== */

// The declared limit is the protocol limit: a frame one byte over it is
// rejected. Under the policy contract there is no longer any way for the pool
// to "have room to spare" — writable_payload() is defined by rx_max_size
// itself — so this now tests the mechanism that remains: the decoder refuses
// a frame that does not fit the span it was given.
void testProtocolLimitIsEnforced()
{
	Pool pool;
	Rx rx(pool);
	static_assert(Rx::max_decoded_size == kMaxDecoded,
	              "Cobs republishes the policy's limit under its own name");

	const auto too_big = payload(0x30, kMaxDecoded + 1);
	const auto fine    = payload(0x50, kMaxDecoded);

	std::vector<uint8_t> wire;
	for (const uint8_t x : cobs_test::encode(too_big)) { wire.push_back(x); }
	for (const uint8_t x : cobs_test::encode(fine))    { wire.push_back(x); }
	rx.consume(std::span<const uint8_t>{wire});

	check(rx.stats().oversize == 1, "a frame one byte over rx_max_size is rejected");
	check(rx.stats().frames_delivered == 1, "and only the legal frame is delivered");
	const Ref r = rx.pop_packet();
	check(r.size() == fine.size(), "which is the one that fits");
	check(pool.rx_available() == kBlocks - 1,
	      "the rejected frame's block went back to the pool");
}

/* ============================ error handling ============================ */

void testMalformedAndRecovery()
{
	Pool pool;
	Rx rx(pool);

	const auto good = payload(0x60, 3);
	std::vector<uint8_t> wire{0x04, 0x11, 0x00}; // promises 3 bytes, delimits after 1
	for (const uint8_t x : cobs_test::encode(good)) { wire.push_back(x); }
	rx.consume(std::span<const uint8_t>{wire});

	check(rx.stats().malformed == 1, "the truncated frame is reported malformed");
	check(rx.stats().frames_delivered == 1, "and the next frame still arrives");
	check(rx.stats().resyncs == 0,
	      "no resync was needed: the delimiter that exposed it already synchronized us");
	const Ref r = rx.pop_packet();
	check(matches(r, good), "with the right contents");
	check(pool.rx_available() == kBlocks - 1, "the malformed frame's block was reclaimed");
}

void testExhaustionDropsFramesAndRecovers()
{
	Pool pool;
	Rx rx(pool);

	const auto p = payload(0x80, 4);
	const auto one = cobs_test::encode(p);
	std::vector<uint8_t> wire;
	for (int i = 0; i < 6; ++i) { // two more frames than the pool has blocks
		for (const uint8_t x : one) { wire.push_back(x); }
	}
	rx.consume(std::span<const uint8_t>{wire});

	check(rx.stats().frames_delivered == kBlocks, "the pool's worth of frames is delivered");
	check(rx.stats().allocation_failure == 2, "the rest fail to allocate");
	check(rx.stats().frames_lost == 2, "and are counted as lost");
	check(pool.rx_available() == 0, "the pool is fully committed to the queued packets");

	// Draining restores capacity and the link keeps working.
	std::vector<Ref> drained;
	while (Ref r = rx.pop_packet()) {
		drained.push_back(std::move(r));
	}
	check(drained.size() == kBlocks, "every queued packet is popped");
	drained.clear();
	check(pool.rx_available() == kBlocks, "releasing them restores the whole pool");

	rx.consume(std::span<const uint8_t>{one});
	const Ref again = rx.pop_packet();
	check(matches(again, p), "and reception continues normally afterwards");
	check(pool.rx_stats().rejected == 0, "no block was ever double-freed");
}

void testGapDropsOnlyTheFrameInFlight()
{
	Pool pool;
	Rx rx(pool);

	const auto first = payload(0x90, 3);
	const auto later = payload(0xA0, 2);

	// A whole frame, then the start of another.
	std::vector<uint8_t> head = cobs_test::encode(first);
	const auto partial = cobs_test::encode(payload(0xB0, 5));
	for (std::size_t i = 0; i + 1 < partial.size(); ++i) { head.push_back(partial[i]); }
	rx.consume(std::span<const uint8_t>{head});
	check(rx.stats().frames_delivered == 1, "the complete frame is queued");

	rx.gap(); // the transport lost bytes here
	check(rx.stats().frames_lost == 1, "the gap costs the frame that was in flight");
	check(pool.rx_available() == kBlocks - 1,
	      "its block is reclaimed; only the queued packet still holds one");

	// Bytes right after a gap must be discarded even if they look like a
	// frame: only a delimiter restores framing.
	const auto bogus = cobs_test::encode(payload(0xC0, 4));
	rx.consume(std::span<const uint8_t>{bogus.data() + 1, bogus.size() - 1});
	rx.consume(std::span<const uint8_t>{cobs_test::encode(later)});

	check(rx.stats().frames_delivered == 2, "exactly one further frame is delivered");
	const Ref a = rx.pop_packet();
	const Ref b = rx.pop_packet();
	check(matches(a, first), "the packet from before the gap is intact");
	check(matches(b, later), "and the next one is the frame that started after resync");
	check(!rx.pop_packet(), "nothing was invented from the damaged region");
}

/* ======================= PacketRef ownership ============================ */

// One frame in the queue, then every handle operation, checked through pool
// occupancy: the block must be held while any handle refers to it and freed
// exactly once when the last one goes.
void testHandleSemantics()
{
	const auto p = payload(0xD0, 6);
	const auto wire = cobs_test::encode(p);

	{	// copy keeps the packet alive past the original
		Pool pool;
		Rx rx(pool);
		feed(rx, wire);
		Ref a = rx.pop_packet();
		check(pool.rx_available() == kBlocks - 1, "a popped packet holds its block");
		{
			Ref b = a;
			a.reset();
			check(pool.rx_available() == kBlocks - 1,
			      "after a copy, releasing the original does not free the packet");
			check(matches(b, p), "and the copy still reads the payload");
		}
		check(pool.rx_available() == kBlocks, "the last handle frees it");
		check(pool.rx_stats().rejected == 0, "exactly once");
	}
	{	// move transfers without changing the count
		Pool pool;
		Rx rx(pool);
		feed(rx, wire);
		Ref a = rx.pop_packet();
		Ref b = std::move(a);
		check(!a, "a moved-from handle is empty");
		check(matches(b, p), "the moved-to handle owns the packet");
		check(pool.rx_available() == kBlocks - 1, "and it is still held exactly once");
		b.reset();
		check(pool.rx_available() == kBlocks, "releasing it frees the block");
	}
	{	// assignment releases what is overwritten
		Pool pool;
		Rx rx(pool);
		feed(rx, wire);
		feed(rx, wire);
		Ref a = rx.pop_packet();
		Ref b = rx.pop_packet();
		check(pool.rx_available() == kBlocks - 2, "two packets held");
		b = a;
		check(pool.rx_available() == kBlocks - 1, "copy assignment released b's packet");
		a.reset();
		check(pool.rx_available() == kBlocks - 1, "b still holds the shared one");
		b.reset();
		check(pool.rx_available() == kBlocks, "and releases it last");

		feed(rx, wire);
		feed(rx, wire);
		Ref c = rx.pop_packet();
		Ref d = rx.pop_packet();
		d = std::move(c);
		check(pool.rx_available() == kBlocks - 1, "move assignment released d's packet");
		check(matches(d, p) && !c, "and transferred c's");
	}
	{	// self-assignment must not free what it is about to keep
		Pool pool;
		Rx rx(pool);
		feed(rx, wire);
		Ref a = rx.pop_packet();
		Ref& alias = a;

		a = alias;
		check(static_cast<bool>(a) && matches(a, p),
		      "self copy-assignment leaves the handle and its payload intact");
		check(pool.rx_available() == kBlocks - 1, "and the block still held");

		a = std::move(alias);
		check(static_cast<bool>(a) && matches(a, p),
		      "self move-assignment leaves the handle and its payload intact");

		a.reset();
		check(pool.rx_available() == kBlocks, "and it still frees exactly once");
		check(pool.rx_stats().rejected == 0, "with no double free");
	}
}

// Whatever CobsRx still owns when it dies must go back to the pool.
void testDestructorReleasesEverything()
{
	Pool pool;
	const auto wire = cobs_test::encode(payload(0xE0, 3));
	{
		Rx rx(pool);
		feed(rx, wire);
		feed(rx, wire);
		// A third frame left half-decoded, so a packet is also mid-build.
		rx.consume(std::span<const uint8_t>{wire.data(), wire.size() - 1});
		check(pool.rx_available() == kBlocks - 3,
		      "two queued packets and one being built hold three blocks");
	}
	check(pool.rx_available() == kBlocks,
	      "destroying CobsRx returns the queued packets and the one in progress");
	check(pool.rx_stats().rejected == 0, "without freeing anything twice");
}

// The application holding packets really does apply back-pressure.
void testRetentionConsumesCapacity()
{
	Pool pool;
	Rx rx(pool);
	const auto wire = cobs_test::encode(payload(0xF0, 2));

	std::vector<Ref> retained;
	for (std::size_t i = 0; i < kBlocks; ++i) {
		feed(rx, wire);
		retained.push_back(rx.pop_packet());
	}
	check(pool.rx_available() == 0, "retained packets consume the pool");

	feed(rx, wire);
	check(rx.stats().allocation_failure == 1,
	      "so a further frame cannot be received while they are held");

	retained.clear();
	check(pool.rx_available() == kBlocks, "releasing them restores capacity");
	feed(rx, wire);
	check(static_cast<bool>(rx.pop_packet()), "and reception resumes");
}

} // namespace

int main()
{
	group("EndToEnd");
	testRoundTripThroughTheWholeStack();
	testSpanBoundaries();

	group("ProtocolLimit");
	testDefaultAllocator();
	testProtocolLimitIsEnforced();

	group("ErrorHandling");
	testMalformedAndRecovery();
	testExhaustionDropsFramesAndRecovers();
	testGapDropsOnlyTheFrameInFlight();

	group("Ownership");
	testHandleSemantics();
	testDestructorReleasesEverything();
	testRetentionConsumesCapacity();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
