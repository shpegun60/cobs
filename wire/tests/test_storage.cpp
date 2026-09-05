/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * The storage contract (doc/STORAGE.md), run against both built-in strategies
 * from one test body, bound to the geometries the two protocols really use.
 *
 * That shared body is the real point. It uses nothing but the contract — a
 * Geometry, four operations on raw bytes — so if it ever needed to know
 * whether it was talking to the heap, to static storage, to COBS or to
 * Modbus, the abstraction would have leaked. Anything a strategy exposes
 * beyond the contract (pool occupancy, statistics, exhaustion counts) is
 * tested separately, where it belongs.
 *
 * The geometries below are stand-ins with a 16-byte header, the size of both
 * protocols' RX block headers on a 32-bit target; the protocols' own suites
 * bind the real ones.
 */
#include "wire/Storage.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <type_traits>
#include <vector>

namespace {

constexpr std::size_t kHeader = 16u;

// What a COBS endpoint with Format<1024> binds, what a Modbus RTU endpoint
// binds, and a cache-line-aligned shape no protocol here uses yet.
using CobsLike = wire::BlockGeometry<
	wire::round_up(kHeader + 1024u, alignof(void*)), 1034u, alignof(void*)>;
using RtuLike = wire::BlockGeometry<
	wire::round_up(kHeader + 256u, alignof(void*)), 256u, alignof(void*)>;
using CacheLine = wire::BlockGeometry<128u, 64u, 64u>;

static_assert(wire::Geometry<CobsLike>);
static_assert(wire::Geometry<RtuLike>);
static_assert(wire::Geometry<CacheLine>);

static_assert(wire::round_up(18u, 4u) == 20u && wire::round_up(16u, 4u) == 16u &&
              wire::round_up(1u, 1u) == 1u && wire::round_up(17u, 1u) == 17u,
	"round_up rounds to the next multiple and leaves multiples alone");

/* ================= the Geometry concept enforces what the doc says ======= */

std::size_t runtime_limit() { return 1040u; }

struct NonConstGeometry {
	static inline std::size_t rx_block_bytes = runtime_limit(); // not a constant expression
	static constexpr std::size_t tx_block_bytes = 1034u;
	static constexpr std::size_t alignment = 4u;
};
struct UnroundedGeometry {
	static constexpr std::size_t rx_block_bytes = 18u; // 18 % 4 != 0: slot 1 would be misaligned
	static constexpr std::size_t tx_block_bytes = 20u;
	static constexpr std::size_t alignment = 4u;
};
struct FractionalGeometry {
	static constexpr double rx_block_bytes = 1040.5;
	static constexpr std::size_t tx_block_bytes = 1034u;
	static constexpr std::size_t alignment = 4u;
};
struct OddAlignmentGeometry {
	static constexpr std::size_t rx_block_bytes = 1040u;
	static constexpr std::size_t tx_block_bytes = 1034u;
	static constexpr std::size_t alignment = 6u;
};
struct ZeroAlignmentGeometry {
	static constexpr std::size_t rx_block_bytes = 1040u;
	static constexpr std::size_t tx_block_bytes = 1034u;
	static constexpr std::size_t alignment = 0u;
};
struct EmptyRxGeometry {
	static constexpr std::size_t rx_block_bytes = 0u;
	static constexpr std::size_t tx_block_bytes = 1034u;
	static constexpr std::size_t alignment = 4u;
};
struct MissingTxGeometry {
	static constexpr std::size_t rx_block_bytes = 1040u;
	static constexpr std::size_t alignment = 4u;
};

static_assert(!wire::Geometry<NonConstGeometry>,
	"a limit a fixed pool cannot size its slabs from at compile time is not a geometry");
static_assert(!wire::Geometry<UnroundedGeometry>,
	"rx_block_bytes must be a multiple of alignment, or slot i of an array is misaligned");
static_assert(!wire::Geometry<FractionalGeometry>,
	"the three numbers are integral");
static_assert(!wire::Geometry<OddAlignmentGeometry>, "alignment is a power of two");
static_assert(!wire::Geometry<ZeroAlignmentGeometry>, "and at least one");
static_assert(!wire::Geometry<EmptyRxGeometry>, "a zero-byte RX block cannot hold a header");
static_assert(!wire::Geometry<MissingTxGeometry>, "all three numbers are required");
static_assert(!wire::Geometry<int>);

/* ================ the ByteStorage / Storage concepts ==================== */

struct MissingTx {
	std::byte* acquire_rx(std::size_t) noexcept;
	void release_rx(std::byte*) noexcept;
};
struct WrongTxResult {
	std::byte* acquire_rx(std::size_t) noexcept;
	void release_rx(std::byte*) noexcept;
	std::byte* acquire_tx(std::size_t) noexcept; // a bare pointer loses the grant
	void release_tx(wire::TxBlock) noexcept;
};
struct ThrowingAcquireRx {
	std::byte* acquire_rx(std::size_t); // deliberately not noexcept
	void release_rx(std::byte*) noexcept;
	wire::TxBlock acquire_tx(std::size_t) noexcept;
	void release_tx(wire::TxBlock) noexcept;
};
struct TypedRx {
	struct Block {};
	Block* acquire_rx(std::size_t) noexcept; // storage hands out bytes, not objects
	void release_rx(Block*) noexcept;
	wire::TxBlock acquire_tx(std::size_t) noexcept;
	void release_tx(wire::TxBlock) noexcept;
};

static_assert(!wire::ByteStorage<MissingTx>, "both TX operations are required");
static_assert(!wire::ByteStorage<WrongTxResult>, "acquire_tx returns the ownership descriptor");
static_assert(!wire::ByteStorage<ThrowingAcquireRx>, "storage operations are noexcept");
static_assert(!wire::ByteStorage<TypedRx>, "RX acquisitions are raw bytes");

// A specification is a type with a nested For<Geometry>; a plain storage is not one.
struct NotASpec {
	std::byte* acquire_rx(std::size_t) noexcept;
	void release_rx(std::byte*) noexcept;
	wire::TxBlock acquire_tx(std::size_t) noexcept;
	void release_tx(wire::TxBlock) noexcept;
};
struct SpecOfMissingTx {
	template<class> using For = MissingTx;
};

static_assert(wire::Storage<wire::Heap, CobsLike>);
static_assert(wire::Storage<wire::Heap, RtuLike>);
static_assert(wire::Storage<wire::Pool<2, 1>, CobsLike>);
static_assert(wire::Storage<wire::Pool<8, 2>, RtuLike>);
static_assert(wire::Storage<wire::Pool<1, 1>, CacheLine>,
	"a fixed pool can promise any power-of-two alignment");
static_assert(!wire::Storage<wire::Heap, CacheLine>,
	"the heap promises only what ::operator new does, and says so as a false concept, not a hard error");
static_assert(!wire::Storage<NotASpec, CobsLike>);
static_assert(!wire::Storage<SpecOfMissingTx, CobsLike>);
static_assert(!wire::Storage<wire::Heap, UnroundedGeometry>,
	"a bad geometry fails the specification check too");

static_assert(WIRE_POOL_CHECKS == 1,
	"pool validation is on by default in every build, NDEBUG included");

int g_checks = 0;
int g_failures = 0;
const char* g_strategy = "";

void group(const char* name) { std::printf("\n[%s]\n", name); }

void check(const bool ok, const std::string& what)
{
	++g_checks;
	if (ok) {
		std::printf("  ok    %s: %s\n", g_strategy, what.c_str());
	} else {
		++g_failures;
		std::printf("  FAIL  %s: %s\n", g_strategy, what.c_str());
	}
}

template<class G>
bool aligned(const void* const p)
{
	return reinterpret_cast<std::uintptr_t>(p) % G::alignment == 0u;
}

/* ====================== the contract, for any strategy ================== */

template<class Spec, class G, bool IndependentQuotas = false>
void runContract(const char* name)
{
	g_strategy = name;
	using S = typename Spec::template For<G>;
	static_assert(wire::ByteStorage<S>);
	S storage;

	// --- RX: raw bytes, at least what was asked, aligned as the geometry demands
	std::byte* const p = storage.acquire_rx(G::rx_block_bytes);
	check(p != nullptr, "acquire_rx(rx_block_bytes) is honoured");
	check(aligned<G>(p), "and the bytes are aligned to Geometry::alignment");
	for (std::size_t i = 0; i < G::rx_block_bytes; ++i) {
		p[i] = static_cast<std::byte>(i);
	}
	bool intact = true;
	for (std::size_t i = 0; i < G::rx_block_bytes; ++i) {
		intact = intact && p[i] == static_cast<std::byte>(i);
	}
	check(intact, "the whole block is writable and reads back");

	std::byte* const q = storage.acquire_rx(G::rx_block_bytes);
	check(q != nullptr && q != p, "a second block is a distinct region");
	check(q != nullptr && aligned<G>(q), "also aligned");
	if (q != nullptr) {
		p[0] = std::byte{0xAA};
		q[0] = std::byte{0x55};
		check(p[0] == std::byte{0xAA} && q[0] == std::byte{0x55}, "and does not overlap the first");
	}

	std::byte* const small = storage.acquire_rx(1u);
	check(small != nullptr && aligned<G>(small), "a one-byte request is honoured and aligned too");

	/* --- TX: a descriptor whose `granted` is a physical promise.
	 *
	 *     requested <= granted
	 *     memory holds at least `granted` writable bytes
	 *
	 * How much slack a strategy grants is its own business — exact, a size
	 * class, or the whole slab — so the shared body never asserts a particular
	 * granted value beyond the lower bound.
	 */
	const auto txRequest = [&storage](const std::size_t requested) {
		const std::string n = std::to_string(requested);
		const wire::TxBlock t = storage.acquire_tx(requested);
		check(t.memory != nullptr, "acquire_tx(" + n + ") is honoured");
		if (t.memory == nullptr) {
			return;
		}
		check(t.granted >= requested, "and grants at least the " + n + " asked for");
		auto* const bytes = t.memory;
		for (std::size_t i = 0; i < t.granted; ++i) {
			bytes[i] = static_cast<std::byte>(i);
		}
		check(bytes[0] == std::byte{0x00} &&
		      bytes[t.granted - 1u] == static_cast<std::byte>(t.granted - 1u),
		      "with every granted byte writable (request " + n + ")");
		storage.release_tx(t);
	};

	txRequest(1u);
	txRequest(7u);
	txRequest(G::tx_block_bytes / 2u);
	txRequest(G::tx_block_bytes);

	const wire::TxBlock t = storage.acquire_tx(G::tx_block_bytes);
	const wire::TxBlock u = storage.acquire_tx(G::tx_block_bytes);
	check(t.memory != nullptr && u.memory != nullptr && u.memory != t.memory,
	      "two TX blocks are distinct");

	// --- RX exhaustion must never starve TX: the quotas are independent.
	if constexpr (IndependentQuotas) {
		std::vector<std::byte*> hoard;
		for (int i = 0; i < 64; ++i) {
			std::byte* const extra = storage.acquire_rx(G::rx_block_bytes);
			if (extra == nullptr) { break; }
			hoard.push_back(extra);
		}
		const wire::TxBlock still = storage.acquire_tx(G::tx_block_bytes);
		check(still.memory != nullptr,
		      "TX still allocates while RX is held to exhaustion (independent quotas)");
		storage.release_tx(still);
		for (std::byte* const h : hoard) { storage.release_rx(h); }
	}

	storage.release_rx(p);
	storage.release_rx(q);
	storage.release_rx(small);
	storage.release_tx(t);
	storage.release_tx(u);

	// --- null is a no-op, not an abuse
	storage.release_rx(nullptr);
	storage.release_tx({});
	check(true, "releasing nullptr / an empty descriptor is harmless");

	// --- churn far beyond any plausible capacity: a leak would run it dry,
	// and a strategy that aligns only its first slot would show up here.
	bool churn_ok = true;
	bool churn_aligned = true;
	for (int i = 0; i < 500; ++i) {
		std::byte* const rx = storage.acquire_rx(G::rx_block_bytes);
		const wire::TxBlock tx = storage.acquire_tx(7u);
		churn_ok = churn_ok && rx != nullptr && tx.memory != nullptr;
		churn_aligned = churn_aligned && rx != nullptr && aligned<G>(rx);
		if (rx != nullptr) { rx[kHeader] = std::byte{0x11}; }
		storage.release_rx(rx);
		storage.release_tx(tx);
	}
	check(churn_ok, "500 acquire/release cycles never run dry");
	check(churn_aligned, "and every RX grant along the way was aligned");
}

/* ==================== strategy-specific, beyond the contract ============ */

void testPoolExhaustionAndIndependence()
{
	g_strategy = "pool";
	using S = wire::Pool<2, 1>::For<RtuLike>;
	S a;

	check(a.rx_available() == 2 && a.tx_available() == 1, "pools start full");
	check(a.acquire_rx(RtuLike::rx_block_bytes + 1u) == nullptr &&
	      a.acquire_tx(RtuLike::tx_block_bytes + 1u).memory == nullptr,
	      "built-in Pool refuses requests beyond Geometry (not a custom-storage obligation)");

	std::byte* const p1 = a.acquire_rx(RtuLike::rx_block_bytes);
	std::byte* const p2 = a.acquire_rx(RtuLike::rx_block_bytes);
	check(p1 != nullptr && p2 != nullptr, "both RX blocks allocate");
	check(a.acquire_rx(RtuLike::rx_block_bytes) == nullptr,
	      "a third returns nullptr rather than an error");
	check(a.rx_stats().exhausted == 1, "and the exhaustion is counted");
	check(a.rx_stats().high_water == 2, "with the high-water mark at the quota");

	const wire::TxBlock t = a.acquire_tx(RtuLike::tx_block_bytes);
	check(t.memory != nullptr, "TX is untouched by RX exhaustion");
	check(a.acquire_tx(RtuLike::tx_block_bytes).memory == nullptr,
	      "and exhausts independently");

	a.release_rx(p1);
	a.release_rx(p1); // double free
	check(a.rx_stats().rejected == 1, "a double free is rejected, not honoured");
	check(a.rx_available() == 1, "and the pool is not corrupted");

	S other;
	std::byte* const foreign = other.acquire_rx(RtuLike::rx_block_bytes);
	a.release_rx(foreign);
	check(a.rx_stats().rejected == 2 && other.rx_stats().in_use == 1,
	      "a block from another pool is refused and stays owned by its own");
	other.release_rx(foreign);

	a.release_rx(p2);
	a.release_tx(t);
	check(a.rx_available() == 2 && a.tx_available() == 1, "everything comes back");
}

/*
 * What a strategy grants is NOT part of the generic contract, so each one is
 * checked here against its own documented rule.
 */
void testHeapGrantsExactlyWhatWasAsked()
{
	g_strategy = "heap";
	wire::Heap::For<CobsLike> a;
	check(a.acquire_rx(CobsLike::rx_block_bytes + 1u) == nullptr &&
	      a.acquire_tx(CobsLike::tx_block_bytes + 1u).memory == nullptr,
	      "built-in Heap refuses requests beyond Geometry");

	// No rounding, no size classes: deciding how much to ask for belongs to
	// the protocol's Message, and a storage with a second opinion would be two
	// growth rules arguing over one allocation.
	bool all_ok = true;
	for (const std::size_t requested : {std::size_t{1}, std::size_t{7}, std::size_t{9},
	                                    std::size_t{70}, std::size_t{500},
	                                    std::size_t{1033}, std::size_t{1034}}) {
		const wire::TxBlock t = a.acquire_tx(requested);
		const bool ok = t.memory != nullptr && t.granted == requested;
		if (!ok) {
			std::printf("       request %zu -> granted %zu\n", requested, t.granted);
		}
		all_ok = all_ok && ok;
		a.release_tx(t);
	}
	check(all_ok, "the heap grants exactly the bytes requested");
}

void testPoolGrantsTheWholeSlab()
{
	g_strategy = "pool";
	wire::Pool<1, 1>::For<CobsLike> a;

	// One size class: every accepted request is granted the whole slab,
	// because the block cost that much whatever was asked for.
	bool all_ok = true;
	for (const std::size_t requested : {std::size_t{1}, std::size_t{7},
	                                    std::size_t{1000}, std::size_t{1034}}) {
		const wire::TxBlock t = a.acquire_tx(requested);
		all_ok = all_ok && t.memory != nullptr && t.granted == CobsLike::tx_block_bytes;
		a.release_tx(t);
	}
	check(all_ok, "a single-slab pool grants tx_block_bytes for every request");
	check(a.tx_available() == 1, "and every block came back");
}

void testPoolHonoursLargeAlignment()
{
	g_strategy = "pool";
	wire::Pool<4, 1>::For<CacheLine> a;

	bool all_aligned = true;
	std::byte* held[4] = {};
	for (std::byte*& h : held) {
		h = a.acquire_rx(CacheLine::rx_block_bytes);
		all_aligned = all_aligned && h != nullptr && aligned<CacheLine>(h);
	}
	check(all_aligned, "every slot of a 64-byte-aligned geometry is 64-byte aligned, not only the first");
	for (std::byte* const h : held) { a.release_rx(h); }
}

void testPoolFootprintFollowsGeometry()
{
	g_strategy = "pool";
	// The same specification costs a different number of bytes under a
	// different geometry: the quotas are ownership counts, the slabs come
	// from the protocol. What it must never do is come in UNDER the sum of
	// its slabs.
	using Big = wire::Pool<8, 2>::For<CobsLike>;
	using Small = wire::Pool<8, 2>::For<RtuLike>;
	static_assert(sizeof(Big) >= 8u * CobsLike::rx_block_bytes + 2u * CobsLike::tx_block_bytes);
	static_assert(sizeof(Small) >= 8u * RtuLike::rx_block_bytes + 2u * RtuLike::tx_block_bytes);
	static_assert(sizeof(Big) > sizeof(Small));
	static_assert(std::is_empty_v<wire::Heap::For<CobsLike>>,
		"the heap strategy has no state and costs an endpoint nothing");
	check(true, "the same Pool<8, 2> is 8+2 slabs of whichever geometry binds it");
	std::printf("       Pool<8,2>::For<CobsLike> = %zu bytes, ::For<RtuLike> = %zu bytes\n",
	            sizeof(Big), sizeof(Small));
}

} // namespace

int main()
{
	group("Contract");
	runContract<wire::Heap, CobsLike, true>("heap/cobs-like");
	runContract<wire::Heap, RtuLike, true>("heap/rtu-like");
	runContract<wire::Pool<8, 4>, CobsLike, true>("pool/cobs-like");
	runContract<wire::Pool<8, 4>, RtuLike, true>("pool/rtu-like");
	runContract<wire::Pool<8, 4>, CacheLine, true>("pool/cache-line");
	runContract<wire::Heap, cobs::Endpoint<>::Geometry, true>("heap/cobs-real");
	runContract<wire::Heap, modbus::rtu::Endpoint<>::Geometry, true>("heap/rtu-real");
	runContract<wire::Pool<8, 4>, cobs::Endpoint<>::Geometry, true>("pool/cobs-real");
	runContract<wire::Pool<8, 4>, modbus::rtu::Endpoint<>::Geometry, true>("pool/rtu-real");

	group("PoolSpecific");
	testPoolExhaustionAndIndependence();
	testPoolGrantsTheWholeSlab();
	testPoolHonoursLargeAlignment();
	testPoolFootprintFollowsGeometry();

	group("HeapSpecific");
	testHeapGrantsExactlyWhatWasAsked();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
