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
#include "CobsFixedAllocator.h"
#include "CobsHeapAllocator.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

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
	Packet* const p = a.allocate_rx();
	check(p != nullptr, "allocate_rx yields a packet");
	check(p->refs == 1 && p->size == 0 && p->next_ready == nullptr,
	      "constructed with one reference and no queue link");
	check(p->owner == &a, "and knows which policy reclaims it");

	const auto payload = p->writable_payload();
	check(payload.size() == Allocator::rx_max_size,
	      "the payload span is exactly rx_max_size");
	check(payload.data() == reinterpret_cast<uint8_t*>(p) + sizeof(Packet),
	      "and sits immediately after the header, in one region");

	for (std::size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>(i);
	}
	bool intact = true;
	for (std::size_t i = 0; i < payload.size(); ++i) {
		intact = intact && payload[i] == static_cast<uint8_t>(i);
	}
	check(intact, "the whole declared payload is writable and reads back");

	Packet* const q = a.allocate_rx();
	check(q != nullptr && q != p, "a second packet is a distinct object");
	{	// Non-overlapping: writing one must not disturb the other.
		const auto pa = p->writable_payload();
		const auto qa = q->writable_payload();
		pa[0] = 0xAA;
		qa[0] = 0x55;
		check(pa[0] == 0xAA && qa[0] == 0x55, "and does not overlap it");
	}

	/* --- TX: a block, plus the payload capacity that block permits.
	 *
	 * The whole obligation, and nothing policy-specific:
	 *
	 *     requested <= capacity <= tx_max_size
	 *     the block holds at least cobs_max_wire_size(capacity) bytes
	 *
	 * How much slack a policy chooses to report is its own business — exact,
	 * a power-of-two class, or the whole slab — so the shared body must never
	 * assert a particular number here.
	 */
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
		// worst-case frame for that many payload bytes must fit.
		const std::size_t physical = cobs_max_wire_size(t.capacity);
		auto* const bytes = reinterpret_cast<uint8_t*>(t.memory);
		for (std::size_t i = 0; i < physical; ++i) { bytes[i] = static_cast<uint8_t>(i); }
		check(bytes[0] == 0x00 && bytes[physical - 1] == static_cast<uint8_t>(physical - 1),
		      "with cobs_max_wire_size(capacity) bytes writable (request " + n + ")");

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
			Packet* const extra = a.allocate_rx();
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
		Packet* const rx = a.allocate_rx();
		const TxAllocation tx = a.allocate_tx(7);
		churn_ok = churn_ok && rx != nullptr && tx.memory != nullptr;
		if (rx != nullptr) { rx->writable_payload()[0] = 0x11; }
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

	auto* const p1 = a.allocate_rx();
	auto* const p2 = a.allocate_rx();
	check(p1 != nullptr && p2 != nullptr, "both RX blocks allocate");
	check(a.allocate_rx() == nullptr, "a third returns null rather than an error");
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

	{	// Zero capacity still needs real storage: cobs_max_wire_size(0) is two
		// bytes, which is the canonical empty frame `01 00`.
		const TxAllocation t = a.allocate_tx(0);
		check(t.memory != nullptr && t.capacity == 0,
		      "a zero request yields a real block of zero payload capacity");
		auto* const bytes = reinterpret_cast<uint8_t*>(t.memory);
		bytes[0] = 0x01;
		bytes[1] = 0x00;
		check(bytes[0] == 0x01 && bytes[1] == 0x00,
		      "big enough to hold the canonical empty frame");
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
	auto* const p = a.allocate_rx();
	check(p != nullptr && p->writable_payload().size() == 1024,
	      "a 1024-byte policy hands out exactly 1024 payload bytes");
	a.deallocate_rx(p);
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

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
