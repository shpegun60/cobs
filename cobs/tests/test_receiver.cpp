/*
 * End-to-end verification of the RX vertical:
 *
 *   encoded bytes -> cobs::codec::Decoder -> storage -> cobs::RxBlock
 *                 -> intrusive ready queue -> PacketRef -> release -> pool
 *
 * PacketRef's own semantics (copy, move, assignment, self-assignment) are
 * tested here rather than beside the pool, because a reference can now only
 * come from its legitimate owner: adopt() is private to Receiver, so a test can
 * no longer mint one by hand — which was exactly the hole being closed.
 *
 * Refcounts are therefore checked BEHAVIOURALLY, through pool occupancy. That
 * is a stronger test than reading the field: it asserts the guarantee the
 * application depends on rather than the bookkeeping behind it.
 */
#include "detail/Receiver.h"
#include "Storage.h"
#include "reference_frame.h"

#include <cstdio>
#include <memory>
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
constexpr std::size_t kBlocks = 4;
using Pool = cobs::Pool<cobs::Format<kMaxDecoded, kMaxDecoded>, kBlocks, 2>;
using Rx = cobs::detail::Receiver<Pool>;
using Ref = Rx::Ref;

// Every frame in this suite is an ENGINE frame: COBS([length][body]). The
// length prefix is the protocol's, not the application's, so it is built once
// here and never spelled out in a test body.
std::vector<uint8_t> engine_frame(const std::vector<uint8_t>& body)
{
	return cobs_test::frame(body, Rx::length_size);
}

std::vector<uint8_t> payload(const uint8_t tag, const std::size_t n)
{
	std::vector<uint8_t> v(n);
	for (std::size_t i = 0; i < n; ++i) {
		v[i] = static_cast<uint8_t>(tag + i);
	}
	return v;
}

template<class R>
bool matches(const R& r, const std::vector<uint8_t>& expected)
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
		const auto b = payload(0x40, 0);   // the empty packet: a declared length of 0
		const auto c = payload(0x70, kMaxDecoded);

		std::vector<uint8_t> wire;
		for (const auto* p : {&a, &b, &c}) {
			for (const uint8_t x : engine_frame(*p)) { wire.push_back(x); }
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
	const auto wire = engine_frame(p);

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

// The endpoint's default heap format also works through the internal receiver.
// Receiver itself has no default argument: detail code must not choose an
// application storage strategy.
void testDefaultHeapFormat()
{
	using DefaultHeap = cobs::Heap<>;
	using DefaultRx = cobs::detail::Receiver<DefaultHeap>;
	static_assert(std::is_same_v<DefaultRx::StorageType, DefaultHeap>);
	static_assert(DefaultRx::max_receive_size == 1024, "with its default limit");

	DefaultHeap storage;
	DefaultRx rx(storage);

	const auto p = payload(0x11, 300);
	// NOT engine_frame(): the default policy's Format limits are 1024, so it speaks a
	// TWO-byte header while the pool used elsewhere in this file speaks one.
	// Feeding it the other engine's frame would be exactly the peer
	// incompatibility §3 warns about.
	static_assert(DefaultRx::length_size == 2, "1024 needs a two-byte length field");
	rx.consume(std::span<const uint8_t>{cobs_test::frame(p, DefaultRx::length_size)});
	const DefaultRx::Ref r = rx.pop_packet();
	check(r.size() == p.size(), "the default heap format receives a frame through Receiver");
}

/* ========================= the protocol limit =========================== */

// The declared limit is the protocol limit: a frame one byte over it is
// rejected. Since the length prefix arrived, that rejection happens from the
// HEADER — two bytes of evidence, before a body segment exists and before the
// storage is asked for anything — rather than by filling a span and finding
// it too small. The block count below is what proves it: the oversize frame
// never took one.
void testProtocolLimitIsEnforced()
{
	Pool pool;
	Rx rx(pool);
	static_assert(Rx::max_receive_size == kMaxDecoded,
	              "Cobs republishes the policy's limit under its own name");

	const auto too_big = payload(0x30, kMaxDecoded + 1);
	const auto fine    = payload(0x50, kMaxDecoded);

	std::vector<uint8_t> wire;
	for (const uint8_t x : engine_frame(too_big)) { wire.push_back(x); }
	for (const uint8_t x : engine_frame(fine))    { wire.push_back(x); }
	rx.consume(std::span<const uint8_t>{wire});

	check(rx.stats().oversize == 1, "a frame one byte over rx_max_size is rejected");
	check(rx.stats().frames_delivered == 1, "and only the legal frame is delivered");
	const Ref r = rx.pop_packet();
	check(r.size() == fine.size(), "which is the one that fits");
	check(pool.rx_available() == kBlocks - 1,
	      "and the oversize frame never took a block at all: only the legal "
	      "one is out");
}

/* ============================ error handling ============================ */

void testMalformedAndRecovery()
{
	Pool pool;
	Rx rx(pool);

	const auto good = payload(0x60, 3);
	std::vector<uint8_t> wire{0x04, 0x11, 0x00}; // promises 3 bytes, delimits after 1
	for (const uint8_t x : engine_frame(good)) { wire.push_back(x); }
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
	const auto one = engine_frame(p);
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
	std::vector<uint8_t> head = engine_frame(first);
	const auto partial = engine_frame(payload(0xB0, 5));
	for (std::size_t i = 0; i + 1 < partial.size(); ++i) { head.push_back(partial[i]); }
	rx.consume(std::span<const uint8_t>{head});
	check(rx.stats().frames_delivered == 1, "the complete frame is queued");

	rx.gap(); // the transport lost bytes here
	check(rx.stats().frames_lost == 1, "the gap costs the frame that was in flight");
	check(pool.rx_available() == kBlocks - 1,
	      "its block is reclaimed; only the queued packet still holds one");

	// Bytes right after a gap must be discarded even if they look like a
	// frame: only a delimiter restores framing.
	const auto bogus = engine_frame(payload(0xC0, 4));
	rx.consume(std::span<const uint8_t>{bogus.data() + 1, bogus.size() - 1});
	rx.consume(std::span<const uint8_t>{engine_frame(later)});

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
	const auto wire = engine_frame(p);

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

// Whatever Receiver still owns when it dies must go back to the pool.
void testDestructorReleasesEverything()
{
	Pool pool;
	const auto wire = engine_frame(payload(0xE0, 3));
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
	      "destroying Receiver returns the queued packets and the one in progress");
	check(pool.rx_stats().rejected == 0, "without freeing anything twice");
}

// The application holding packets really does apply back-pressure.
void testRetentionConsumesCapacity()
{
	Pool pool;
	Rx rx(pool);
	const auto wire = engine_frame(payload(0xF0, 2));

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


/* ============================ the length field =========================== */

/*
 * The wire header, checked against hand-written bytes rather than against the
 * codec that produced them.
 */
void testLengthCodec()
{
	using Narrow = cobs::Format<200, 100>;   // both limits below 256
	using Wide   = cobs::Format<1024, 64>;   // one above: two bytes both ways
	using Wide2  = cobs::Format<64, 1024>;   // ...whichever direction it is

	static_assert(Narrow::length_size == 1);
	static_assert(Wide::length_size == 2);
	static_assert(Wide2::length_size == 2);
	static_assert(cobs::Format<0, 0>::length_size == 1, "zero limits still use one byte");
	static_assert(cobs::Format<254, 0>::length_size == 1, "254 fits one byte");
	static_assert(cobs::Format<255, 255>::length_size == 1, "255 still fits one byte");
	static_assert(cobs::Format<256, 1>::length_size == 2, "256 does not");
	static_assert(cobs::Format<65535, 65535>::length_size == 2,
	              "the largest supported limit uses two bytes");
	static_assert(std::is_same_v<Narrow::LengthType, uint8_t>);
	static_assert(std::is_same_v<Wide::LengthType, uint16_t>);
	check(true, "the header width follows max(rx_max_size, tx_max_size)");

	// The asymmetric pair keeps its directional limits; only the wire header
	// is shared.
	static_assert(cobs::detail::Receiver<cobs::Heap<cobs::Format<1024, 64>>>::max_receive_size == 1024);
	static_assert(cobs::detail::Receiver<cobs::Heap<cobs::Format<64, 1024>>>::max_receive_size == 64);
	check(true, "while the RX and TX limits stay independent");

	bool narrow_ok = true;
	for (const std::size_t n : {std::size_t{0}, std::size_t{1},
	                            std::size_t{254}, std::size_t{255}}) {
		uint8_t buf[1] = {0xAA};
		Narrow::store_length(buf, n);
		narrow_ok = narrow_ok && buf[0] == static_cast<uint8_t>(n) &&
		            Narrow::load_length(buf) == n;
	}
	check(narrow_ok, "a one-byte length is the byte itself, and round-trips");

	const struct { std::size_t n; uint8_t lo, hi; } wide_cases[] = {
		{0, 0x00, 0x00}, {1, 0x01, 0x00}, {255, 0xFF, 0x00}, {256, 0x00, 0x01},
		{0x1234, 0x34, 0x12}, {1024, 0x00, 0x04}, {65535, 0xFF, 0xFF},
	};
	bool wide_ok = true;
	for (const auto& c : wide_cases) {
		uint8_t buf[2] = {0xAA, 0xBB};
		Wide::store_length(buf, c.n);
		wide_ok = wide_ok && buf[0] == c.lo && buf[1] == c.hi &&
		          Wide::load_length(buf) == c.n;
	}
	check(wide_ok, "a two-byte length is little-endian, explicitly, and round-trips");
}

/* ===================== every way a length can be wrong =================== */

// A policy that records what RX sizes were asked for, so "exact allocation"
// is a measurement rather than a claim.
class RecordingHeap final {
public:
	using Format = cobs::Format<64, 64>;
	using RxBlock = cobs::RxBlock<RecordingHeap>;

	[[nodiscard]] RxBlock* acquire_rx(const std::size_t requested_size) noexcept
	{
		if (requested_size > Format::max_receive_size || refuse) {
			return nullptr;
		}
		void* const memory =
			::operator new(sizeof(RxBlock) + requested_size, std::nothrow);
		if (memory == nullptr) { return nullptr; }
		requests.push_back(requested_size);
		return std::construct_at(static_cast<RxBlock*>(memory));
	}
	void release_rx(RxBlock* const block) noexcept
	{
		if (block == nullptr) { return; }
		++frees;
		std::destroy_at(block);
		::operator delete(static_cast<void*>(block));
	}
	[[nodiscard]] cobs::TxBlock acquire_tx(std::size_t) noexcept { return {}; }
	void release_tx(cobs::TxBlock) noexcept {}

	std::vector<std::size_t> requests;
	std::size_t frees = 0;
	bool refuse = false;
};

using RecRx = cobs::detail::Receiver<RecordingHeap>;

void testExactAllocation()
{
	static_assert(RecRx::length_size == 1, "64/64 needs only a one-byte header");
	RecordingHeap pool;
	RecRx rx(pool);

	// Nothing is allocated until the declared length is known — and then
	// exactly that much, once.
	for (const std::size_t n : {std::size_t{1}, std::size_t{7}, std::size_t{37},
	                            std::size_t{63}, RecordingHeap::Format::max_receive_size}) {
		const auto body = payload(0x40, n);
		rx.consume(std::span<const uint8_t>{cobs_test::frame(body, RecRx::length_size)});
		const auto r = rx.pop_packet();
		check(r.size() == n && std::vector<uint8_t>(r.data().begin(), r.data().end()) == body,
		      "a " + std::to_string(n) + "-byte frame arrives intact");
	}
	const std::vector<std::size_t> expected{1, 7, 37, 63, RecordingHeap::Format::max_receive_size};
	check(pool.requests == expected,
	      "and each was allocated at EXACTLY its declared length, once");

	{	// A zero-length application packet is a real packet, and costs one
		// allocation of zero payload bytes.
		pool.requests.clear();
		rx.consume(std::span<const uint8_t>{cobs_test::frame({}, RecRx::length_size)});
		const auto r = rx.pop_packet();
		check(static_cast<bool>(r) && r.size() == 0,
		      "an empty application payload is delivered as a zero-length packet");
		check(pool.requests == std::vector<std::size_t>{0},
		      "asking the policy for zero payload bytes");
	}
}

void testMalformedLengths()
{
	const auto counts = [](const RecRx& rx) { return rx.stats(); };

	{	// A frame whose decoded content is empty: no header at all. That is
		// the old pure-COBS `01 00`, which is no longer an engine frame.
		RecordingHeap pool;
		RecRx rx(pool);
		rx.consume(std::span<const uint8_t>{cobs_test::frame_of_decoded({})});
		check(counts(rx).length_mismatch == 1 && counts(rx).frames_delivered == 0,
		      "a frame with no length field at all is a length mismatch");
		check(pool.requests.empty(), "and allocates nothing");
		check(counts(rx).resyncs == 0,
		      "the delimiter that exposed it already resynchronized us");
	}
	{	// A truncated two-byte header.
		using Wide = cobs::Heap<cobs::Format<1024, 1024>>;
		Wide pool;
		cobs::detail::Receiver<Wide> rx(pool);
		static_assert(cobs::detail::Receiver<Wide>::length_size == 2);
		rx.consume(std::span<const uint8_t>{cobs_test::frame_of_decoded({0x05})});
		check(rx.stats().length_mismatch == 1 && rx.stats().frames_delivered == 0,
		      "half a two-byte header is a length mismatch");
	}
	{	// Declared zero, then body bytes anyway.
		RecordingHeap pool;
		RecRx rx(pool);
		rx.consume(std::span<const uint8_t>{
			cobs_test::frame_declaring(0, payload(0x50, 4), RecRx::length_size)});
		check(rx.stats().length_mismatch == 1, "a declared zero followed by data is rejected");
		check(pool.requests.empty(), "with nothing allocated");
		check(rx.stats().resyncs == 1, "and the rest of the frame has to be skipped");
	}
	{	// Declared above rx_max_size: refused before any allocation.
		RecordingHeap pool;
		RecRx rx(pool);
		rx.consume(std::span<const uint8_t>{
			cobs_test::frame_declaring(RecordingHeap::Format::max_receive_size + 1,
			                           payload(0x60, 40), RecRx::length_size)});
		check(rx.stats().oversize == 1 && rx.stats().frames_delivered == 0,
		      "a declared length above rx_max_size is oversize");
		check(pool.requests.empty(), "and never reaches storage");
		check(rx.stats().resyncs == 1, "the remaining bytes are discarded to the delimiter");
	}
	{	// Body shorter than declared: found BY the delimiter, so no resync.
		RecordingHeap pool;
		RecRx rx(pool);
		rx.consume(std::span<const uint8_t>{
			cobs_test::frame_declaring(10, payload(0x70, 4), RecRx::length_size)});
		check(rx.stats().length_mismatch == 1 && rx.stats().frames_delivered == 0,
		      "a body shorter than declared is a length mismatch");
		check(pool.requests == std::vector<std::size_t>{10} && pool.frees == 1,
		      "the packet it allocated is returned");
		check(rx.stats().resyncs == 0, "and the delimiter already resynchronized us");
	}
	{	// Body longer than declared: found BEFORE the delimiter, so resync.
		RecordingHeap pool;
		RecRx rx(pool);
		rx.consume(std::span<const uint8_t>{
			cobs_test::frame_declaring(4, payload(0x80, 12), RecRx::length_size)});
		check(rx.stats().length_mismatch == 1 && rx.stats().frames_delivered == 0,
		      "a body longer than declared is a length mismatch");
		check(pool.requests == std::vector<std::size_t>{4} && pool.frees == 1,
		      "the packet is returned");
		check(rx.stats().resyncs == 1, "and the rest has to be skipped");
	}
	{	// Allocation failure after a perfectly good header.
		RecordingHeap pool;
		RecRx rx(pool);
		pool.refuse = true;
		rx.consume(std::span<const uint8_t>{
			cobs_test::frame(payload(0x90, 20), RecRx::length_size)});
		check(rx.stats().allocation_failure == 1 && rx.stats().frames_delivered == 0,
		      "an allocation failure after a valid header is counted as such");
		check(rx.stats().length_mismatch == 0, "and not blamed on the length");
		pool.refuse = false;
		rx.consume(std::span<const uint8_t>{
			cobs_test::frame(payload(0xA0, 6), RecRx::length_size)});
		check(rx.stats().frames_delivered == 1, "and the next frame still arrives");
	}
	{	// A gap in the middle of the header, and one in the middle of a body.
		// After either, the decoder must hunt for a delimiter before it can
		// trust anything — so the frame that follows the gap is consumed by
		// the resync, and the one after that is the first to arrive. That is
		// the pre-existing gap contract, not something the length field
		// changes.
		RecordingHeap pool;
		RecRx rx(pool);
		const auto wire = cobs_test::frame(payload(0xB0, 20), RecRx::length_size);
		const auto next = cobs_test::frame(payload(0xC0, 5), RecRx::length_size);
		const auto after = cobs_test::frame(payload(0xE0, 3), RecRx::length_size);

		rx.consume(std::span<const uint8_t>{wire.data(), 1}); // header started
		rx.gap();
		rx.consume(std::span<const uint8_t>{next});   // eaten by the resync
		rx.consume(std::span<const uint8_t>{after});
		check(rx.stats().frames_delivered == 1,
		      "a gap during the header costs that frame and the resync's");
		check(matches(rx.pop_packet(), payload(0xE0, 3)),
		      "and the first frame after the resync is intact");

		rx.consume(std::span<const uint8_t>{wire.data(), 6}); // into the body
		rx.gap();
		rx.consume(std::span<const uint8_t>{next});   // eaten by the resync
		rx.consume(std::span<const uint8_t>{after});
		check(rx.stats().frames_delivered == 2, "and a gap during the body behaves the same");
		check(matches(rx.pop_packet(), payload(0xE0, 3)),
		      "with the recovered frame byte-for-byte correct");
		check(pool.frees >= 1, "and the half-built packet was returned to the policy");
	}
}

// The fixed policy answers a small request from a full slab and still
// publishes only what was declared.
void testFixedSlabPublishesDeclaredLength()
{
	Pool pool;
	Rx rx(pool);
	const auto body = payload(0x30, 7);
	rx.consume(std::span<const uint8_t>{engine_frame(body)});
	const auto r = rx.pop_packet();
	check(r.size() == 7, "a fixed policy publishes the declared 7 bytes");
	check(matches(r, body), "with exactly those bytes");
	check(pool.rx_available() == kBlocks - 1, "out of one whole slab");
}


/*
 * RX lengths around every boundary that matters. The header shifts the COBS
 * block boundaries by length_size, so both the raw lengths (253/254/255) and
 * the lengths that PUT the header-inclusive size on them are swept — that
 * off-by-H is exactly what a frame format bolted onto an encoder gets wrong.
 */
void testRxLengthSweep()
{
	using Wide = cobs::Heap<cobs::Format<600, 600>>;   // two-byte header
	static_assert(cobs::detail::Receiver<Wide>::length_size == 2);
	Wide pool;
	cobs::detail::Receiver<Wide> rx(pool);

	std::vector<std::size_t> lengths{0, 1, 2, 3};
	for (const std::size_t n : {252u, 253u, 254u, 255u, 256u, 507u, 508u, 509u}) {
		lengths.push_back(n);
	}
	lengths.push_back(Wide::Format::max_receive_size - 1);
	lengths.push_back(Wide::Format::max_receive_size);

	bool all_ok = true;
	std::size_t cases = 0;
	for (const std::size_t n : lengths) {
		for (int pattern_id = 0; pattern_id < 3; ++pattern_id) {
			std::vector<uint8_t> body(n);
			for (std::size_t i = 0; i < n; ++i) {
				switch (pattern_id) {
				case 0: body[i] = static_cast<uint8_t>(1 + (i % 255)); break; // zero-free
				case 1: body[i] = 0; break;                                  // all zeros
				default: body[i] = static_cast<uint8_t>((i % 4 == 1) ? 0 : (0x41 + (i % 60)));
				}
			}
			const auto wire = cobs_test::frame(body, cobs::detail::Receiver<Wide>::length_size);
			rx.consume(std::span<const uint8_t>{wire});
			const auto r = rx.pop_packet();
			all_ok = all_ok && r.size() == n &&
			         std::vector<uint8_t>(r.data().begin(), r.data().end()) == body;
			++cases;
		}
	}
	check(all_ok, std::to_string(cases) + " RX lengths x patterns across every "
	              "COBS boundary, header included, arrive byte for byte");
	check(rx.stats().frames_delivered == cases && rx.stats().frames_lost == 0,
	      "with nothing lost along the way");

	{	// One byte past the limit is refused from the header alone.
		rx.consume(std::span<const uint8_t>{
			cobs_test::frame_declaring(Wide::Format::max_receive_size + 1,
			                           std::vector<uint8_t>(10, 0x55),
			                           cobs::detail::Receiver<Wide>::length_size)});
		check(rx.stats().oversize == 1, "and rx_max_size + 1 is refused from the header");
	}
}


/*
 * The one allocation that happens AFTER the delimiter.
 *
 * A frame declaring zero needs no body segment, so nothing is allocated until
 * FrameComplete — by which point the delimiter is already consumed and the
 * stream is synchronized. A failure there must therefore cost a frame and NO
 * resync, unlike every other allocation failure, which is discovered with the
 * rest of the frame still in the stream.
 */
class RefuseEmptyOnly final {
public:
	using Format = cobs::Format<64, 64>;
	using RxBlock = cobs::RxBlock<RefuseEmptyOnly>;

	[[nodiscard]] RxBlock* acquire_rx(const std::size_t requested_size) noexcept
	{
		if (requested_size == 0u || requested_size > Format::max_receive_size) {
			return nullptr; // the empty packet, and only it, is refused
		}
		void* const memory =
			::operator new(sizeof(RxBlock) + requested_size, std::nothrow);
		if (memory == nullptr) { return nullptr; }
		return std::construct_at(static_cast<RxBlock*>(memory));
	}
	void release_rx(RxBlock* const block) noexcept
	{
		if (block == nullptr) { return; }
		std::destroy_at(block);
		::operator delete(static_cast<void*>(block));
	}
	[[nodiscard]] cobs::TxBlock acquire_tx(std::size_t) noexcept { return {}; }
	void release_tx(cobs::TxBlock) noexcept {}
};

void testEmptyPacketAllocationFailure()
{
	RefuseEmptyOnly pool;
	cobs::detail::Receiver<RefuseEmptyOnly> rx(pool);

	rx.consume(std::span<const uint8_t>{
		cobs_test::frame({}, cobs::detail::Receiver<RefuseEmptyOnly>::length_size)});
	check(rx.stats().allocation_failure == 1 && rx.stats().frames_delivered == 0,
	      "a refused empty packet is an allocation failure");
	check(rx.stats().frames_lost == 1, "and costs the frame");
	check(rx.stats().resyncs == 0,
	      "but NO resync: that allocation happens after the delimiter, so the "
	      "stream is already synchronized");

	// The very next frame must arrive without needing a delimiter first, which
	// is the observable consequence of not having resynced.
	const auto body = payload(0x21, 5);
	rx.consume(std::span<const uint8_t>{
		cobs_test::frame(body, cobs::detail::Receiver<RefuseEmptyOnly>::length_size)});
	check(rx.stats().frames_delivered == 1, "and the next frame arrives immediately");
	check(matches(rx.pop_packet(), body), "with its bytes intact");
}


/* ============ a header that is oversize and has no body at all ========== */

/*
 * Oversize is a verdict on the HEADER, so it has to be reached the same way
 * whether or not a body ever started. It is easy for it not to be: a frame
 * declaring 65 with one body byte takes the beginBody() path, while the same
 * frame with no body reaches FrameComplete directly and, before this was
 * fixed, was counted as a plain length mismatch. The classification would then
 * have depended on the frame's punctuation rather than on its header.
 *
 * It is also the symmetric case to a refused empty packet: the delimiter has
 * already arrived, so it costs no resync.
 */
void testOversizeWithNoBody()
{
	RecordingHeap pool;
	RecRx rx(pool);

	rx.consume(std::span<const uint8_t>{
		cobs_test::frame_declaring(RecordingHeap::Format::max_receive_size + 1, {},
		                           RecRx::length_size)});
	check(rx.stats().oversize == 1,
	      "a declared length above the limit is oversize even with no body");
	check(rx.stats().length_mismatch == 0, "and is not filed as a length mismatch");
	check(rx.stats().frames_lost == 1, "the frame is lost");
	check(rx.stats().resyncs == 0,
	      "with no resync: the delimiter arrived before the verdict did");
	check(pool.requests.empty(), "and storage was never asked");

	// No resync means the very next frame arrives without a delimiter first.
	const auto body = payload(0x31, 6);
	rx.consume(std::span<const uint8_t>{cobs_test::frame(body, RecRx::length_size)});
	check(rx.stats().frames_delivered == 1, "and the next frame arrives immediately");
	check(matches(rx.pop_packet(), body), "intact");

	{	// The same header WITH a body must land on the same counter, by the
		// other path — that is the whole point.
		RecordingHeap pool2;
		RecRx rx2(pool2);
		rx2.consume(std::span<const uint8_t>{
			cobs_test::frame_declaring(RecordingHeap::Format::max_receive_size + 1,
			                           payload(0x41, 8), RecRx::length_size)});
		check(rx2.stats().oversize == 1 && rx2.stats().length_mismatch == 0,
		      "the same header with a body is the same verdict");
		check(rx2.stats().resyncs == 1,
		      "differing only in the resync, since that one is found early");
	}
}

/* ================== the packet's internals are unreachable ============== */

// Compile-time, because a runtime test cannot check that something does NOT
// compile. Each of these was reachable at some point, and each was a way to
// read or write past an exact allocation, or to corrupt the refcount.
template<class P>
concept CanWritePayload = requires(P& p) { p.writable_payload(std::size_t{1}); };
template<class P>
concept CanSetSize = requires(P& p) { p.size = uint16_t{1}; };
template<class P>
concept CanReadRefs = requires(const P& p) { p.refs; };
template<class P>
concept CanRelink = requires(P& p) { p.next_ready = nullptr; };
template<class P>
concept CanRetarget = requires(P& p) { p.owner = nullptr; };
template<class P>
concept CanReadData = requires(const P& p) { p.data(); };

void testPacketInternalsAreSealed()
{
	using P = cobs::RxBlock<cobs::Heap<cobs::Format<1024, 1024>>>;
	static_assert(!CanWritePayload<P>, "the writable span must not be reachable");
	static_assert(!CanSetSize<P>,
	              "a public size is a one-line out-of-bounds read: "
	              "acquire_rx(20), size = 1024, data()[1000]");
	static_assert(!CanReadRefs<P>, "the refcount is not public bookkeeping");
	static_assert(!CanRelink<P>, "nor is the ready-queue link");
	static_assert(!CanRetarget<P>, "nor the pointer that decides who frees it");
	static_assert(CanReadData<P>, "while the immutable view stays public");
	check(true, "cobs::RxBlock exposes data() and nothing else");
}


/* ============ a policy written to the letter of the contract ============ */

/*
 * §9 says storage names Format and RxBlock and provides four memory
 * operations. This one is exactly that and not a byte more — it never touches
 * packet->owner, because nothing in the contract says to.
 *
 * It used to segfault. The shipped policies both stamped `owner` themselves,
 * so the obligation was real but written down nowhere, invisible in the
 * signatures, and — once the field became private — impossible for the
 * contract test to check. A policy that read the contract and believed it
 * handed back a packet whose owner was null, and the first PacketRef release
 * dereferenced it.
 *
 * Receiver sets `owner` now, which is where establishing ownership belongs
 * anyway: storage supplies memory and takes it back.
 */
class MinimalStorage final {
public:
	using Format = cobs::Format<64, 64>;
	using RxBlock = cobs::RxBlock<MinimalStorage>;

	[[nodiscard]] RxBlock* acquire_rx(const std::size_t requested_size) noexcept
	{
		if (requested_size > Format::max_receive_size) { return nullptr; }
		void* const memory =
			::operator new(sizeof(RxBlock) + requested_size, std::nothrow);
		if (memory == nullptr) { return nullptr; }
		++live;
		return std::construct_at(static_cast<RxBlock*>(memory));
	}
	void release_rx(RxBlock* const block) noexcept
	{
		if (block == nullptr) { return; }
		--live;
		++frees;
		std::destroy_at(block);
		::operator delete(static_cast<void*>(block));
	}
	[[nodiscard]] cobs::TxBlock acquire_tx(std::size_t) noexcept { return {}; }
	void release_tx(cobs::TxBlock) noexcept {}

	int live = 0;
	int frees = 0;
};

static_assert(cobs::Storage<MinimalStorage>);

void testContractIsSelfSufficient()
{
	MinimalStorage pool;
	cobs::detail::Receiver<MinimalStorage> rx(pool);

	const auto body = payload(0x51, 3);
	rx.consume(std::span<const uint8_t>{
		cobs_test::frame(body, cobs::detail::Receiver<MinimalStorage>::length_size)});
	check(rx.stats().frames_delivered == 1,
	      "a policy that is only the four contract functions receives a frame");
	check(pool.live == 1, "holding one packet");

	{
		const auto r = rx.pop_packet();
		check(matches(r, body), "which pops with the right bytes");
		check(pool.frees == 0, "and is not freed while the reference lives");
	}
	// The release path goes through owner->release_rx(). If nothing had
	// stamped owner, this is where it would have dereferenced null.
	check(pool.frees == 1 && pool.live == 0,
	      "and goes back to the policy when the last reference drops");

	{	// The empty-packet path allocates from a different place, so it needs
		// its own proof that ownership was established there too.
		rx.consume(std::span<const uint8_t>{
			cobs_test::frame({}, cobs::detail::Receiver<MinimalStorage>::length_size)});
		const auto r = rx.pop_packet();
		check(static_cast<bool>(r) && r.size() == 0, "an empty packet arrives too");
	}
	check(pool.frees == 2 && pool.live == 0, "and is reclaimed the same way");
}


/* ========================= the refcount's width ========================= */

/*
 * PacketRef's copy constructor does `++refs` with no check, so the counter's
 * WIDTH is the only thing preventing a wrap — and a wrap is a
 * use-after-free, not a leak:
 *
 *     N copies of one reference   stored refs = (1 + N) mod 2^bits
 *     at N == 2^bits              stored refs is 1 again
 *     destroy any ONE of them     refs 1 -> 0, the packet is freed
 *     the other 2^bits handles are still alive and still pointing at it
 *
 * With a 16-bit counter that took 65536 copies of one packet, reachable
 * through nothing but the public API, and ASan called it exactly what it was.
 * The loop below is 70000 — comfortably past where the old width broke, and
 * nowhere near where the current one does.
 */
void testRefcountDoesNotWrap()
{
	using LocalRef = typename RecRx::Ref;
	RecordingHeap pool;
	RecRx rx(pool);

	const auto body = payload(0x61, 4);
	rx.consume(std::span<const uint8_t>{cobs_test::frame(body, RecRx::length_size)});
	LocalRef original = rx.pop_packet();
	check(static_cast<bool>(original), "one packet, one reference");

	constexpr std::size_t kCopies = 70000; // > 65536, the old wrap point
	{
		std::vector<LocalRef> copies;
		copies.reserve(kCopies);
		for (std::size_t i = 0; i < kCopies; ++i) {
			copies.emplace_back(original);
		}
		check(pool.frees == 0, "70000 copies free nothing");

		// The old bug fired here: dropping one reference out of 70001 freed
		// the packet, because the stored count had wrapped back to a small
		// number on the way up.
		copies.pop_back();
		check(pool.frees == 0, "and dropping one of them frees nothing either");

		const auto d = original.data();
		check(d.size() == body.size() &&
		      std::vector<uint8_t>(d.begin(), d.end()) == body,
		      "the original still reads its own bytes, not freed memory");
	}
	check(pool.frees == 0, "the packet outlives every copy but the original");
	original = LocalRef{};
	check(pool.frees == 1, "and is freed exactly once when the last one goes");
}

/* ==================== the widest frames the format allows =============== */

/*
 * The edges of the length field itself. The last case is the interesting one:
 *
 *     body          = 65535   fits cobs::RxBlock::size, a uint16_t, exactly
 *     decoded frame = 65537   does NOT
 *
 * so any code that casts the DECODED size rather than the body would truncate
 * to 1 here and deliver a one-byte packet. Receiver subtracts the header before
 * the cast; this is the frame that proves it.
 */
template<std::size_t Max>
void checkWidestFrame(const char* name)
{
	using WideHeap = cobs::Heap<cobs::Format<Max, Max>>;
	WideHeap storage;
	cobs::detail::Receiver<WideHeap> rx(storage);

	std::vector<uint8_t> body(Max);
	for (std::size_t i = 0; i < Max; ++i) {
		// Zeros at irregular intervals, so the COBS blocks are not all 0xFF.
		body[i] = static_cast<uint8_t>((i % 251 == 7) ? 0 : (1 + (i % 254)));
	}
	rx.consume(std::span<const uint8_t>{cobs_test::frame(body, cobs::detail::Receiver<WideHeap>::length_size)});

	const auto r = rx.pop_packet();
	const bool ok = r.size() == Max &&
	                std::vector<uint8_t>(r.data().begin(), r.data().end()) == body;
	check(ok, std::string(name) + ": a body of " + std::to_string(Max) +
	          " bytes round-trips whole (decoded frame " +
	          std::to_string(Max + cobs::detail::Receiver<WideHeap>::length_size) + ")");
	check(rx.stats().frames_delivered == 1 && rx.stats().frames_lost == 0,
	      std::string(name) + ": with nothing lost");
}

void testWidestFrames()
{
	static_assert(cobs::detail::Receiver<cobs::Heap<cobs::Format<255, 255>>>::length_size == 1,
	              "255 is the last body a one-byte header can describe");
	static_assert(cobs::detail::Receiver<cobs::Heap<cobs::Format<256, 256>>>::length_size == 2,
	              "256 needs two");
	static_assert(cobs::detail::Receiver<cobs::Heap<cobs::Format<65535, 65535>>>::length_size == 2,
	              "and two is enough to the very end");

	checkWidestFrame<255>("H=1 max");
	checkWidestFrame<256>("H=2 just over");
	checkWidestFrame<65535>("H=2 max");
}

} // namespace

int main()
{
	group("LengthField");
	testLengthCodec();
	testExactAllocation();
	testMalformedLengths();
	testFixedSlabPublishesDeclaredLength();
	testRxLengthSweep();
	testEmptyPacketAllocationFailure();
	testOversizeWithNoBody();
	testPacketInternalsAreSealed();
	testContractIsSelfSufficient();
	testRefcountDoesNotWrap();
	testWidestFrames();

	group("EndToEnd");
	testRoundTripThroughTheWholeStack();
	testSpanBoundaries();

	group("ProtocolLimit");
	testDefaultHeapFormat();
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
