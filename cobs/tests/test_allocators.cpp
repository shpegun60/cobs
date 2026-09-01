/*
 * The allocator policy contract (doc/COBS_ENGINE.md §9), run against BOTH
 * policies from one test body.
 *
 * That shared body is the real point. It uses nothing but the contract — two
 * constants and four functions — so if it ever needed to know whether it was
 * talking to the heap or to static storage, the abstraction would have leaked.
 * Anything a policy exposes beyond the contract (pool occupancy, statistics,
 * exhaustion counts) is tested separately, where it belongs.
 */
#include "Storage.h"
#include "CobsFixedAllocator.h"
#include "CobsHeapAllocator.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using ContractHeap = CobsHeapAllocator<128, 96>;
using ContractPool = CobsFixedAllocator<128, 2, 96, 1>;

static_assert(cobs::Storage<ContractHeap>,
	"the built-in heap policy must satisfy the checked storage contract");
static_assert(cobs::Storage<ContractPool>,
	"the built-in fixed policy must satisfy the checked storage contract");

struct MissingTxOperations {
	struct Packet {};
	static constexpr std::size_t rx_max_size = 8;
	static constexpr std::size_t tx_max_size = 8;

	Packet* allocate_rx(std::size_t) noexcept;
	void deallocate_rx(Packet*) noexcept;
};

struct WrongTxResult {
	struct Packet {};
	static constexpr std::size_t rx_max_size = 8;
	static constexpr std::size_t tx_max_size = 8;

	Packet* allocate_rx(std::size_t) noexcept;
	void deallocate_rx(Packet*) noexcept;
	std::byte* allocate_tx(std::size_t) noexcept;
	void deallocate_tx(std::byte*, std::size_t) noexcept;
};

struct ThrowingAllocateRx {
	struct Packet {};
	static constexpr std::size_t rx_max_size = 8;
	static constexpr std::size_t tx_max_size = 8;

	Packet* allocate_rx(std::size_t); // deliberately not noexcept
	void deallocate_rx(Packet*) noexcept;
	TxAllocation allocate_tx(std::size_t) noexcept;
	void deallocate_tx(std::byte*, std::size_t) noexcept;
};

static_assert(!cobs::Storage<MissingTxOperations>,
	"a storage implementation must provide both TX operations");
static_assert(!cobs::Storage<WrongTxResult>,
	"allocate_tx must return the ownership descriptor, not a bare pointer");
static_assert(!cobs::Storage<ThrowingAllocateRx>,
	"storage operations are required to be noexcept");

int g_checks = 0;
int g_failures = 0;
const char* g_policy = "";

void group(const char* name) { std::printf("\n[%s]\n", name); }

void check(const bool ok, const std::string& what)
{
	++g_checks;
	if (ok) {
		std::printf("  ok    %s: %s\n", g_policy, what.c_str());
	} else {
		++g_failures;
		std::printf("  FAIL  %s: %s\n", g_policy, what.c_str());
	}
}

/* ====================== the contract, for any policy ==================== */

template<class Allocator>
void runContract(const char* name)
{
	g_policy = name;
	using Packet = typename Allocator::Packet;
	Allocator a;

	// --- RX: one contiguous [RxPacket][payload], sized by the declared limit
	Packet* const p = a.allocate_rx(Allocator::rx_max_size);
	check(p != nullptr, "allocate_rx yields a packet");
	check(p->data().empty(), "carrying no decoded bytes yet");
	// refs, size, next_ready and owner are private to the RX vertical (§6.5) —
	// a public `size` was a one-line out-of-bounds read once allocations
	// became exact. They are asserted where they can be asserted honestly:
	// through behaviour, in test_cobs_rx, where a released PacketRef returning
	// its block to the right pool is the only proof that `owner` is right that
	// does not consist of reading `owner`.

	// writable_payload() is private to the RX vertical (§6.5), so the contract
	// test reaches the storage the way the contract DEFINES it: one contiguous
	// [RxPacket][payload] region. Reading it back through the public data()
	// view is what proves the two descriptions are the same bytes.
	const auto payload_of = [](Packet* const pk) {
		return reinterpret_cast<uint8_t*>(pk) + sizeof(Packet);
	};
	const std::span<uint8_t> payload{payload_of(p), Allocator::rx_max_size};
	check(payload.size() == Allocator::rx_max_size,
	      "a packet allocated at rx_max_size has that many payload bytes");

	for (std::size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>(i);
	}
	bool intact = true;
	for (std::size_t i = 0; i < payload.size(); ++i) {
		intact = intact && payload[i] == static_cast<uint8_t>(i);
	}
	check(intact, "the whole declared payload is writable and reads back");

	Packet* const q = a.allocate_rx(Allocator::rx_max_size);
	check(q != nullptr && q != p, "a second packet is a distinct object");
	{	// Non-overlapping: writing one must not disturb the other.
		payload_of(p)[0] = 0xAA;
		payload_of(q)[0] = 0x55;
		check(payload_of(p)[0] == 0xAA && payload_of(q)[0] == 0x55,
		      "and does not overlap it");
	}

	/* --- TX: a block, plus the payload capacity that block permits.
	 *
	 * The whole obligation, and nothing policy-specific:
	 *
	 *     requested <= capacity <= tx_max_size
	 *     the block holds at least
	 *         Format::tx_storage_size_for_capacity(capacity) bytes
	 *
	 * That last line is HEADER-INCLUSIVE, and it is the whole reason this
	 * check exists. The encoded frame is [length][payload], so a policy that
	 * still sizes its blocks with cobs_max_wire_size(capacity) — correct
	 * before the length prefix, one or two bytes short after it — would sail
	 * through a test written the old way and then have CobsMsg::encode() run
	 * off the end of the block. Asking Format for the number is what makes
	 * that regression impossible to miss.
	 *
	 * How much slack a policy chooses to report is still its own business —
	 * exact, a size class, or the whole slab — so the shared body must never
	 * assert a particular capacity here.
	 */
	using Format = CobsFormatFor<Allocator>;
	const auto txRequest = [&a](const std::size_t requested) {
		const std::string n = std::to_string(requested);
		const TxAllocation t = a.allocate_tx(requested);
		check(t.memory != nullptr, "allocate_tx(" + n + ") is honoured");
		if (t.memory == nullptr) {
			return;
		}
		check(t.capacity >= requested, "and reports at least the " + n + " asked for");
		check(t.capacity <= Allocator::tx_max_size,
		      "and never more than tx_max_size (request " + n + ")");

		// The reported capacity is a promise about physical storage: the
		// worst-case frame for that many payload bytes must fit — INCLUDING
		// the length header that shares the block with them.
		const std::size_t physical = Format::tx_storage_size_for_capacity(t.capacity);
		auto* const bytes = reinterpret_cast<uint8_t*>(t.memory);
		for (std::size_t i = 0; i < physical; ++i) { bytes[i] = static_cast<uint8_t>(i); }
		check(bytes[0] == 0x00 && bytes[physical - 1] == static_cast<uint8_t>(physical - 1),
		      "with the whole header-inclusive block writable (request " + n + ")");

		a.deallocate_tx(t.memory, t.capacity);
	};

	txRequest(0);
	txRequest(1);
	txRequest(7);
	txRequest(Allocator::tx_max_size / 2);
	txRequest(Allocator::tx_max_size);

	{	// The declared limit is a limit: one byte past it is not negotiable.
		const TxAllocation over = a.allocate_tx(Allocator::tx_max_size + 1);
		check(over.memory == nullptr && over.capacity == 0,
		      "a request beyond tx_max_size yields an empty allocation");
	}

	const TxAllocation t = a.allocate_tx(Allocator::tx_max_size);
	const TxAllocation u = a.allocate_tx(Allocator::tx_max_size);
	check(t.memory != nullptr && u.memory != nullptr && u.memory != t.memory,
	      "two TX blocks are distinct");
	check(t.capacity == Allocator::tx_max_size,
	      "a request for the whole limit reports exactly that capacity");

	// --- §9.1.2: RX exhaustion must never starve TX
	{
		std::vector<Packet*> hoard;
		for (int i = 0; i < 64; ++i) {
			Packet* const extra = a.allocate_rx(Allocator::rx_max_size);
			if (extra == nullptr) { break; }
			hoard.push_back(extra);
		}
		const TxAllocation still = a.allocate_tx(Allocator::tx_max_size);
		check(still.memory != nullptr,
		      "TX still allocates while RX is held to exhaustion (independent quotas)");
		a.deallocate_tx(still.memory, still.capacity);
		for (Packet* const h : hoard) { a.deallocate_rx(h); }
	}

	a.deallocate_rx(p);
	a.deallocate_rx(q);
	a.deallocate_tx(t.memory, t.capacity);
	a.deallocate_tx(u.memory, u.capacity);

	// --- null is a no-op, not an abuse
	a.deallocate_rx(nullptr);
	a.deallocate_tx(nullptr, 0);
	check(true, "deallocating nullptr is harmless");

	// --- churn far beyond any plausible capacity: a leak would run it dry.
	// Deliberately returned with the capacity the policy reported, which is
	// the only number a segregated policy could use to find the block's pool.
	bool churn_ok = true;
	for (int i = 0; i < 500; ++i) {
		Packet* const rx = a.allocate_rx(Allocator::rx_max_size);
		const TxAllocation tx = a.allocate_tx(7);
		churn_ok = churn_ok && rx != nullptr && tx.memory != nullptr;
		if (rx != nullptr) { reinterpret_cast<uint8_t*>(rx)[sizeof(Packet)] = 0x11; }
		a.deallocate_rx(rx);
		a.deallocate_tx(tx.memory, tx.capacity);
	}
	check(churn_ok, "500 allocate/free cycles never run dry");
}

/* ==================== policy-specific, beyond the contract ============== */

void testFixedExhaustionAndIndependence()
{
	g_policy = "fixed";
	using Allocator = CobsFixedAllocator<64, 2, 32, 1>;
	Allocator a;

	check(a.rx_available() == 2 && a.tx_available() == 1, "pools start full");

	auto* const p1 = a.allocate_rx(Allocator::rx_max_size);
	auto* const p2 = a.allocate_rx(Allocator::rx_max_size);
	check(p1 != nullptr && p2 != nullptr, "both RX blocks allocate");
	check(a.allocate_rx(Allocator::rx_max_size) == nullptr, "a third returns null rather than an error");
	check(a.rx_stats().exhausted == 1, "and the exhaustion is counted");

	const TxAllocation t = a.allocate_tx(Allocator::tx_max_size);
	check(t.memory != nullptr, "TX is untouched by RX exhaustion");
	check(a.allocate_tx(Allocator::tx_max_size).memory == nullptr,
	      "and exhausts independently");

	a.deallocate_rx(p1);
	a.deallocate_rx(p1); // double free
	check(a.rx_stats().rejected == 1, "a double free is rejected, not honoured");
	check(a.rx_available() == 1, "and the pool is not corrupted");

	a.deallocate_rx(p2);
	a.deallocate_tx(t.memory, t.capacity);
	check(a.rx_available() == 2 && a.tx_available() == 1, "everything comes back");
}

/*
 * The capacity a policy reports is NOT part of the generic contract, so each
 * one is checked here, against its own documented rule. This is the half of
 * the model the shared body must stay ignorant of.
 */
void testHeapGrantsExactlyWhatWasAsked()
{
	g_policy = "heap";
	using Allocator = CobsHeapAllocator<64, 1024>;
	Allocator a;

	// No rounding, no size classes, no growth rule: deciding how much to ask
	// for belongs to CobsMsg (§9.1.0), and a policy with a second opinion
	// would be two growth rules arguing over one allocation.
	bool all_ok = true;
	for (const std::size_t requested : {std::size_t{0}, std::size_t{1}, std::size_t{7},
	                                    std::size_t{9}, std::size_t{70}, std::size_t{500},
	                                    std::size_t{1023}, std::size_t{1024}}) {
		const TxAllocation t = a.allocate_tx(requested);
		const bool ok = t.memory != nullptr && t.capacity == requested;
		if (!ok) {
			std::printf("       request %zu -> capacity %zu\n", requested, t.capacity);
		}
		all_ok = all_ok && ok;
		a.deallocate_tx(t.memory, t.capacity);
	}
	check(all_ok, "the heap policy reports exactly the capacity requested");

	{	// Zero capacity still needs real storage — and more of it than it used
		// to. The canonical empty ENGINE frame is not `01 00` any more: its
		// decoded content is the length field, so the block must hold
		// cobs_max_wire_size(length_size), three bytes for a one-byte header
		// and four for a two-byte one.
		using Fmt = CobsFormatFor<Allocator>;
		const TxAllocation t = a.allocate_tx(0);
		check(t.memory != nullptr && t.capacity == 0,
		      "a zero request yields a real block of zero payload capacity");
		const std::size_t needed = Fmt::tx_storage_size_for_capacity(0);
		check(needed == cobs_max_wire_size(Fmt::length_size),
		      "sized for the length field alone, delimiter included");
		auto* const bytes = reinterpret_cast<uint8_t*>(t.memory);
		for (std::size_t i = 0; i < needed; ++i) { bytes[i] = static_cast<uint8_t>(0xA0 + i); }
		check(bytes[needed - 1] == static_cast<uint8_t>(0xA0 + needed - 1),
		      "and every one of those bytes is writable");
		a.deallocate_tx(t.memory, t.capacity);
	}
}

void testFixedReportsTheWholeSlab()
{
	g_policy = "fixed";
	using Allocator = CobsFixedAllocator<64, 1, 1024, 1>;
	Allocator a;

	// One size class: every accepted request reports the whole slab, because
	// the block cost that much whatever was asked for.
	bool all_ok = true;
	for (const std::size_t requested : {std::size_t{0}, std::size_t{1}, std::size_t{7},
	                                    std::size_t{1000}, std::size_t{1024}}) {
		const TxAllocation t = a.allocate_tx(requested);
		all_ok = all_ok && t.memory != nullptr && t.capacity == 1024;
		a.deallocate_tx(t.memory, t.capacity);
	}
	check(all_ok, "a single-slab policy reports tx_max_size for every request");
	check(a.tx_available() == 1, "and every block came back");
}

void testFixedGeometryIsAbiIndependent()
{
	g_policy = "fixed";
	// The declared limits are exactly what was asked for, on any ABI; only the
	// physical block size moves, and it is nobody's business (§9.1.1).
	using Allocator = CobsFixedAllocator<1024, 4, 256, 2>;
	static_assert(Allocator::rx_max_size == 1024);
	static_assert(Allocator::tx_max_size == 256);

	Allocator a;
	auto* const p = a.allocate_rx(Allocator::rx_max_size);
	check(p != nullptr, "a 1024-byte policy honours a 1024-byte request");
	auto* const bytes = reinterpret_cast<uint8_t*>(p) + sizeof(*p);
	for (std::size_t i = 0; i < 1024; ++i) { bytes[i] = static_cast<uint8_t>(i); }
	bool readable = true;
	for (std::size_t i = 0; i < 1024; ++i) {
		readable = readable && bytes[i] == static_cast<uint8_t>(i);
	}
	check(readable, "with all 1024 payload bytes usable on any ABI");
	a.deallocate_rx(p);
}


/*
 * The size arithmetic is unsigned, so a pathological limit wraps: at
 * n = SIZE_MAX, cobs_max_wire_size() returns 0 and cobs_raw_offset() returns
 * 1 — a block smaller than the payload meant to go in it, and a headroom
 * below the encoder's minimum. Nobody writes tx_max_size = SIZE_MAX on
 * purpose, but "surely nobody would" is not a correctness argument, so the
 * policies static_assert the guard and such a configuration does not compile.
 */
void testSizeArithmeticGuard()
{
	g_policy = "arithmetic";
	constexpr std::size_t kMax = static_cast<std::size_t>(-1);

	static_assert(cobs_size_arithmetic_fits(0));
	static_assert(cobs_size_arithmetic_fits(1));
	static_assert(cobs_size_arithmetic_fits(1024));
	static_assert(cobs_size_arithmetic_fits(1u << 20));
	check(true, "every plausible limit passes the guard");

	static_assert(!cobs_size_arithmetic_fits(kMax));
	static_assert(!cobs_size_arithmetic_fits(kMax - 1u));
	static_assert(!cobs_size_arithmetic_fits(kMax / 255u * 254u));
	// Note that kMax / 2 does NOT wrap and is correctly accepted: half of
	// size_t plus its own 1/254 still fits. The guard rejects what actually
	// overflows, not everything that merely looks alarming.
	static_assert(cobs_size_arithmetic_fits(kMax / 2u));
	check(true, "the values whose arithmetic wraps are rejected, and only those");

	// The guard must be exactly at the boundary, not conservatively early, so
	// the boundary is searched for rather than guessed: the largest accepted
	// value has to survive the real functions, and the next one must not be
	// accepted at all.
	constexpr std::size_t kLargest = []() constexpr {
		std::size_t lo = 0u;
		std::size_t hi = kMax;
		while (lo < hi) {
			// Rounds up WITHOUT forming hi - lo + 1, which wraps to zero on
			// the very first step of a search over the whole of size_t and
			// leaves the loop spinning on mid == lo forever.
			const std::size_t mid = hi - (hi - lo) / 2u;
			if (cobs_size_arithmetic_fits(mid)) { lo = mid; } else { hi = mid - 1u; }
		}
		return lo;
	}();
	static_assert(cobs_size_arithmetic_fits(kLargest));
	static_assert(!cobs_size_arithmetic_fits(kLargest + 1u),
	              "the guard is tight, not conservative");
	static_assert(cobs_max_wire_size(kLargest) > kLargest,
	              "an accepted limit still yields a block larger than its payload");
	static_assert(cobs_raw_offset(kLargest) >= 2u,
	              "and headroom at least the encoder's minimum");
	check(true, "the boundary is exact, and its geometry is still sane");

	// And the policies really do carry the assertion.
	static_assert(cobs_size_arithmetic_fits(CobsHeapAllocator<64, 1024>::tx_max_size));
	static_assert(cobs_size_arithmetic_fits(
		CobsFixedAllocator<64, 2, 1024, 2>::tx_max_size));
	check(true, "both shipped policies assert it on their own limits");
}

} // namespace

int main()
{
	group("Contract");
	runContract<CobsHeapAllocator<128, 96>>("heap");
	runContract<CobsFixedAllocator<128, 8, 96, 4>>("fixed");

	group("FixedSpecific");
	testFixedExhaustionAndIndependence();
	testFixedGeometryIsAbiIndependent();

	group("ReportedCapacity");
	testHeapGrantsExactlyWhatWasAsked();
	testFixedReportsTheWholeSlab();

	group("SizeArithmetic");
	testSizeArithmeticGuard();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
