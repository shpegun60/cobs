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

	// --- TX: raw bytes big enough for the worst-case frame
	const std::size_t big = cobs_max_wire_size(Allocator::tx_max_size);
	std::byte* const t = a.allocate_tx(big);
	check(t != nullptr, "allocate_tx yields a block");
	{
		auto* const bytes = reinterpret_cast<uint8_t*>(t);
		for (std::size_t i = 0; i < big; ++i) { bytes[i] = static_cast<uint8_t>(i); }
		check(bytes[0] == 0 && bytes[big - 1] == static_cast<uint8_t>(big - 1),
		      "with at least the requested bytes usable");
	}
	std::byte* const u = a.allocate_tx(big);
	check(u != nullptr && u != t, "a second TX block is distinct");

	{	// A small request must be honoured too, whatever the policy does with
		// it physically: exact on a heap, a whole slab on a single-class pool.
		std::byte* const small = a.allocate_tx(cobs_max_wire_size(7));
		check(small != nullptr, "a small request is honoured");
		auto* const bytes = reinterpret_cast<uint8_t*>(small);
		for (std::size_t i = 0; i < cobs_max_wire_size(7); ++i) { bytes[i] = 0x5A; }
		a.deallocate_tx(small, cobs_max_wire_size(7));
	}

	// --- §9.1.2: RX exhaustion must never starve TX
	{
		std::vector<Packet*> hoard;
		for (int i = 0; i < 64; ++i) {
			Packet* const extra = a.allocate_rx();
			if (extra == nullptr) { break; }
			hoard.push_back(extra);
		}
		std::byte* const still = a.allocate_tx(big);
		check(still != nullptr,
		      "TX still allocates while RX is held to exhaustion (independent quotas)");
		a.deallocate_tx(still, big);
		for (Packet* const h : hoard) { a.deallocate_rx(h); }
	}

	a.deallocate_rx(p);
	a.deallocate_rx(q);
	a.deallocate_tx(t, big);
	a.deallocate_tx(u, big);

	// --- null is a no-op, not an abuse
	a.deallocate_rx(nullptr);
	a.deallocate_tx(nullptr, big);
	check(true, "deallocating nullptr is harmless");

	// --- churn far beyond any plausible capacity: a leak would run it dry
	bool churn_ok = true;
	for (int i = 0; i < 500; ++i) {
		Packet* const rx = a.allocate_rx();
		std::byte* const tx = a.allocate_tx(big);
		churn_ok = churn_ok && rx != nullptr && tx != nullptr;
		if (rx != nullptr) { rx->writable_payload()[0] = 0x11; }
		a.deallocate_rx(rx);
		a.deallocate_tx(tx, big);
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

	const std::size_t w = cobs_max_wire_size(Allocator::tx_max_size);
	std::byte* const t = a.allocate_tx(w);
	check(t != nullptr, "TX is untouched by RX exhaustion");
	check(a.allocate_tx(w) == nullptr, "and exhausts independently");

	a.deallocate_rx(p1);
	a.deallocate_rx(p1); // double free
	check(a.rx_stats().rejected == 1, "a double free is rejected, not honoured");
	check(a.rx_available() == 1, "and the pool is not corrupted");

	a.deallocate_rx(p2);
	a.deallocate_tx(t, w);
	check(a.rx_available() == 2 && a.tx_available() == 1, "everything comes back");
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

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
