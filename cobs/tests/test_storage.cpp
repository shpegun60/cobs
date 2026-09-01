/*
 * The storage contract (doc/COBS_ENGINE.md §9), run against both built-in
 * strategies from one test body.
 *
 * That shared body is the real point. It uses nothing but the contract — one
 * Format, one RxBlock type and four operations — so if it ever needed to know whether it was
 * talking to the heap or to static storage, the abstraction would have leaked.
 * Anything a policy exposes beyond the contract (pool occupancy, statistics,
 * exhaustion counts) is tested separately, where it belongs.
 */
#include "Storage.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

using ContractHeap = cobs::Heap<cobs::Format<128, 96>>;
using ContractPool = cobs::Pool<cobs::Format<128, 96>, 2, 1>;

static_assert(cobs::Storage<ContractHeap>,
	"the built-in heap policy must satisfy the checked storage contract");
static_assert(cobs::Storage<ContractPool>,
	"the built-in fixed policy must satisfy the checked storage contract");

struct MissingTxOperations {
	using Format = cobs::Format<8, 8>;
	using RxBlock = cobs::RxBlock<MissingTxOperations>;

	RxBlock* acquire_rx(std::size_t) noexcept;
	void release_rx(RxBlock*) noexcept;
};

struct WrongTxResult {
	using Format = cobs::Format<8, 8>;
	using RxBlock = cobs::RxBlock<WrongTxResult>;

	RxBlock* acquire_rx(std::size_t) noexcept;
	void release_rx(RxBlock*) noexcept;
	std::byte* acquire_tx(std::size_t) noexcept;
	void release_tx(cobs::TxBlock) noexcept;
};

struct ThrowingAcquireRx {
	using Format = cobs::Format<8, 8>;
	using RxBlock = cobs::RxBlock<ThrowingAcquireRx>;

	RxBlock* acquire_rx(std::size_t); // deliberately not noexcept
	void release_rx(RxBlock*) noexcept;
	cobs::TxBlock acquire_tx(std::size_t) noexcept;
	void release_tx(cobs::TxBlock) noexcept;
};

struct MissingFormat {
	using RxBlock = cobs::RxBlock<MissingFormat>;

	RxBlock* acquire_rx(std::size_t) noexcept;
	void release_rx(RxBlock*) noexcept;
	cobs::TxBlock acquire_tx(std::size_t) noexcept;
	void release_tx(cobs::TxBlock) noexcept;
};

struct WrongRxBlock {
	struct RxBlock {};
	using Format = cobs::Format<8, 8>;

	RxBlock* acquire_rx(std::size_t) noexcept;
	void release_rx(RxBlock*) noexcept;
	cobs::TxBlock acquire_tx(std::size_t) noexcept;
	void release_tx(cobs::TxBlock) noexcept;
};

static_assert(!cobs::Storage<MissingTxOperations>,
	"a storage implementation must provide both TX operations");
static_assert(!cobs::Storage<WrongTxResult>,
	"acquire_tx must return the ownership descriptor, not a bare pointer");
static_assert(!cobs::Storage<ThrowingAcquireRx>,
	"storage operations are required to be noexcept");
static_assert(!cobs::Storage<MissingFormat>,
	"a storage implementation must name its protocol Format");
static_assert(!cobs::Storage<WrongRxBlock>,
	"RX ownership must use the typed cobs::RxBlock<Storage> contract");

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

/* ====================== the contract, for any policy ==================== */

template<class StorageT>
void runContract(const char* name)
{
	g_strategy = name;
	using Block = typename StorageT::RxBlock;
	StorageT storage;

	// --- RX: one contiguous [cobs::RxBlock][payload], sized by the declared limit
	Block* const p = storage.acquire_rx(StorageT::Format::max_receive_size);
	check(p != nullptr, "acquire_rx yields a packet");
	check(p->data().empty(), "carrying no decoded bytes yet");
	// refs, size, next_ready and owner are private to the RX vertical (§6.5) —
	// a public `size` was a one-line out-of-bounds read once allocations
	// became exact. They are asserted where they can be asserted honestly:
	// through behaviour, in test_receiver, where a released cobs::Packet returning
	// its block to the right pool is the only proof that `owner` is right that
	// does not consist of reading `owner`.

	// writable_payload() is private to the RX vertical (§6.5), so the contract
	// test reaches the storage the way the contract DEFINES it: one contiguous
	// [cobs::RxBlock][payload] region. Reading it back through the public data()
	// view is what proves the two descriptions are the same bytes.
	const auto payload_of = [](Block* const block) {
		return reinterpret_cast<uint8_t*>(block) + sizeof(Block);
	};
	const std::span<uint8_t> payload{payload_of(p), StorageT::Format::max_receive_size};
	check(payload.size() == StorageT::Format::max_receive_size,
	      "a packet allocated at rx_max_size has that many payload bytes");

	for (std::size_t i = 0; i < payload.size(); ++i) {
		payload[i] = static_cast<uint8_t>(i);
	}
	bool intact = true;
	for (std::size_t i = 0; i < payload.size(); ++i) {
		intact = intact && payload[i] == static_cast<uint8_t>(i);
	}
	check(intact, "the whole declared payload is writable and reads back");

	Block* const q = storage.acquire_rx(StorageT::Format::max_receive_size);
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
	 * still sizes its blocks with cobs::codec::max_wire_size(capacity) — correct
	 * before the length prefix, one or two bytes short after it — would sail
	 * through a test written the old way and then have Message encoding run
	 * off the end of the block. Asking Format for the number is what makes
	 * that regression impossible to miss.
	 *
	 * How much slack a policy chooses to report is still its own business —
	 * exact, a size class, or the whole slab — so the shared body must never
	 * assert a particular capacity here.
	 */
	using Format = typename StorageT::Format;
	const auto txRequest = [&storage](const std::size_t requested) {
		const std::string n = std::to_string(requested);
		const cobs::TxBlock t = storage.acquire_tx(requested);
		check(t.memory != nullptr, "acquire_tx(" + n + ") is honoured");
		if (t.memory == nullptr) {
			return;
		}
		check(t.capacity >= requested, "and reports at least the " + n + " asked for");
		check(t.capacity <= StorageT::Format::max_send_size,
		      "and never more than tx_max_size (request " + n + ")");

		// The reported capacity is a promise about physical storage: the
		// worst-case frame for that many payload bytes must fit — INCLUDING
		// the length header that shares the block with them.
		const std::size_t physical = Format::tx_storage_size_for_capacity(t.capacity);
		auto* const bytes = reinterpret_cast<uint8_t*>(t.memory);
		for (std::size_t i = 0; i < physical; ++i) { bytes[i] = static_cast<uint8_t>(i); }
		check(bytes[0] == 0x00 && bytes[physical - 1] == static_cast<uint8_t>(physical - 1),
		      "with the whole header-inclusive block writable (request " + n + ")");

		storage.release_tx(t);
	};

	txRequest(0);
	txRequest(1);
	txRequest(7);
	txRequest(StorageT::Format::max_send_size / 2);
	txRequest(StorageT::Format::max_send_size);

	{	// The declared limit is a limit: one byte past it is not negotiable.
		const cobs::TxBlock over = storage.acquire_tx(StorageT::Format::max_send_size + 1);
		check(over.memory == nullptr && over.capacity == 0,
		      "a request beyond tx_max_size yields an empty allocation");
	}

	const cobs::TxBlock t = storage.acquire_tx(StorageT::Format::max_send_size);
	const cobs::TxBlock u = storage.acquire_tx(StorageT::Format::max_send_size);
	check(t.memory != nullptr && u.memory != nullptr && u.memory != t.memory,
	      "two TX blocks are distinct");
	check(t.capacity == StorageT::Format::max_send_size,
	      "a request for the whole limit reports exactly that capacity");

	// --- §9.1.2: RX exhaustion must never starve TX
	{
		std::vector<Block*> hoard;
		for (int i = 0; i < 64; ++i) {
			Block* const extra = storage.acquire_rx(StorageT::Format::max_receive_size);
			if (extra == nullptr) { break; }
			hoard.push_back(extra);
		}
		const cobs::TxBlock still = storage.acquire_tx(StorageT::Format::max_send_size);
		check(still.memory != nullptr,
		      "TX still allocates while RX is held to exhaustion (independent quotas)");
		storage.release_tx(still);
		for (Block* const h : hoard) { storage.release_rx(h); }
	}

	storage.release_rx(p);
	storage.release_rx(q);
	storage.release_tx(t);
	storage.release_tx(u);

	// --- null is a no-op, not an abuse
	storage.release_rx(nullptr);
	storage.release_tx({});
	check(true, "deallocating nullptr is harmless");

	// --- churn far beyond any plausible capacity: a leak would run it dry.
	// Deliberately returned with the capacity the policy reported, which is
	// the only number a segregated policy could use to find the block's pool.
	bool churn_ok = true;
	for (int i = 0; i < 500; ++i) {
		Block* const rx = storage.acquire_rx(StorageT::Format::max_receive_size);
		const cobs::TxBlock tx = storage.acquire_tx(7);
		churn_ok = churn_ok && rx != nullptr && tx.memory != nullptr;
		if (rx != nullptr) { reinterpret_cast<uint8_t*>(rx)[sizeof(Block)] = 0x11; }
		storage.release_rx(rx);
		storage.release_tx(tx);
	}
	check(churn_ok, "500 allocate/free cycles never run dry");
}

/* ==================== policy-specific, beyond the contract ============== */

void testFixedExhaustionAndIndependence()
{
	g_strategy = "fixed";
	using Strategy = cobs::Pool<cobs::Format<64, 32>, 2, 1>;
	Strategy a;

	check(a.rx_available() == 2 && a.tx_available() == 1, "pools start full");

	auto* const p1 = a.acquire_rx(Strategy::Format::max_receive_size);
	auto* const p2 = a.acquire_rx(Strategy::Format::max_receive_size);
	check(p1 != nullptr && p2 != nullptr, "both RX blocks allocate");
	check(a.acquire_rx(Strategy::Format::max_receive_size) == nullptr, "a third returns null rather than an error");
	check(a.rx_stats().exhausted == 1, "and the exhaustion is counted");

	const cobs::TxBlock t = a.acquire_tx(Strategy::Format::max_send_size);
	check(t.memory != nullptr, "TX is untouched by RX exhaustion");
	check(a.acquire_tx(Strategy::Format::max_send_size).memory == nullptr,
	      "and exhausts independently");

	a.release_rx(p1);
	a.release_rx(p1); // double free
	check(a.rx_stats().rejected == 1, "a double free is rejected, not honoured");
	check(a.rx_available() == 1, "and the pool is not corrupted");

	a.release_rx(p2);
	a.release_tx(t);
	check(a.rx_available() == 2 && a.tx_available() == 1, "everything comes back");
}

/*
 * The capacity a policy reports is NOT part of the generic contract, so each
 * one is checked here, against its own documented rule. This is the half of
 * the model the shared body must stay ignorant of.
 */
void testHeapGrantsExactlyWhatWasAsked()
{
	g_strategy = "heap";
	using Strategy = cobs::Heap<cobs::Format<64, 1024>>;
	Strategy a;

	// No rounding, no size classes, no growth rule: deciding how much to ask
	// for belongs to cobs::Message (§9.1.0), and a policy with a second opinion
	// would be two growth rules arguing over one allocation.
	bool all_ok = true;
	for (const std::size_t requested : {std::size_t{0}, std::size_t{1}, std::size_t{7},
	                                    std::size_t{9}, std::size_t{70}, std::size_t{500},
	                                    std::size_t{1023}, std::size_t{1024}}) {
		const cobs::TxBlock t = a.acquire_tx(requested);
		const bool ok = t.memory != nullptr && t.capacity == requested;
		if (!ok) {
			std::printf("       request %zu -> capacity %zu\n", requested, t.capacity);
		}
		all_ok = all_ok && ok;
		a.release_tx(t);
	}
	check(all_ok, "the heap policy reports exactly the capacity requested");

	{	// Zero capacity still needs real storage — and more of it than it used
		// to. The canonical empty ENGINE frame is not `01 00` any more: its
		// decoded content is the length field, so the block must hold
		// cobs::codec::max_wire_size(length_size), three bytes for a one-byte header
		// and four for a two-byte one.
		using Fmt = typename Strategy::Format;
		const cobs::TxBlock t = a.acquire_tx(0);
		check(t.memory != nullptr && t.capacity == 0,
		      "a zero request yields a real block of zero payload capacity");
		const std::size_t needed = Fmt::tx_storage_size_for_capacity(0);
		check(needed == cobs::codec::max_wire_size(Fmt::length_size),
		      "sized for the length field alone, delimiter included");
		auto* const bytes = reinterpret_cast<uint8_t*>(t.memory);
		for (std::size_t i = 0; i < needed; ++i) { bytes[i] = static_cast<uint8_t>(0xA0 + i); }
		check(bytes[needed - 1] == static_cast<uint8_t>(0xA0 + needed - 1),
		      "and every one of those bytes is writable");
		a.release_tx(t);
	}
}

void testFixedReportsTheWholeSlab()
{
	g_strategy = "fixed";
	using Strategy = cobs::Pool<cobs::Format<64, 1024>, 1, 1>;
	Strategy a;

	// One size class: every accepted request reports the whole slab, because
	// the block cost that much whatever was asked for.
	bool all_ok = true;
	for (const std::size_t requested : {std::size_t{0}, std::size_t{1}, std::size_t{7},
	                                    std::size_t{1000}, std::size_t{1024}}) {
		const cobs::TxBlock t = a.acquire_tx(requested);
		all_ok = all_ok && t.memory != nullptr && t.capacity == 1024;
		a.release_tx(t);
	}
	check(all_ok, "a single-slab policy reports tx_max_size for every request");
	check(a.tx_available() == 1, "and every block came back");
}

void testFixedGeometryIsAbiIndependent()
{
	g_strategy = "fixed";
	// The declared limits are exactly what was asked for, on any ABI; only the
	// physical block size moves, and it is nobody's business (§9.1.1).
	using Strategy = cobs::Pool<cobs::Format<1024, 256>, 4, 2>;
	static_assert(Strategy::Format::max_receive_size == 1024);
	static_assert(Strategy::Format::max_send_size == 256);

	Strategy a;
	auto* const p = a.acquire_rx(Strategy::Format::max_receive_size);
	check(p != nullptr, "a 1024-byte policy honours a 1024-byte request");
	auto* const bytes = reinterpret_cast<uint8_t*>(p) + sizeof(*p);
	for (std::size_t i = 0; i < 1024; ++i) { bytes[i] = static_cast<uint8_t>(i); }
	bool readable = true;
	for (std::size_t i = 0; i < 1024; ++i) {
		readable = readable && bytes[i] == static_cast<uint8_t>(i);
	}
	check(readable, "with all 1024 payload bytes usable on any ABI");
	a.release_rx(p);
}


/*
 * The size arithmetic is unsigned, so a pathological limit wraps: at
 * n = SIZE_MAX, cobs::codec::max_wire_size() returns 0 and cobs::codec::raw_offset() returns
 * 1 — a block smaller than the payload meant to go in it, and a headroom
 * below the encoder's minimum. Nobody writes tx_max_size = SIZE_MAX on
 * purpose, but "surely nobody would" is not a correctness argument, so the
 * policies static_assert the guard and such a configuration does not compile.
 */
void testSizeArithmeticGuard()
{
	g_strategy = "arithmetic";
	constexpr std::size_t kMax = static_cast<std::size_t>(-1);

	static_assert(cobs::codec::size_arithmetic_fits(0));
	static_assert(cobs::codec::size_arithmetic_fits(1));
	static_assert(cobs::codec::size_arithmetic_fits(1024));
	static_assert(cobs::codec::size_arithmetic_fits(1u << 20));
	check(true, "every plausible limit passes the guard");

	static_assert(!cobs::codec::size_arithmetic_fits(kMax));
	static_assert(!cobs::codec::size_arithmetic_fits(kMax - 1u));
	static_assert(!cobs::codec::size_arithmetic_fits(kMax / 255u * 254u));
	// Note that kMax / 2 does NOT wrap and is correctly accepted: half of
	// size_t plus its own 1/254 still fits. The guard rejects what actually
	// overflows, not everything that merely looks alarming.
	static_assert(cobs::codec::size_arithmetic_fits(kMax / 2u));
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
			if (cobs::codec::size_arithmetic_fits(mid)) { lo = mid; } else { hi = mid - 1u; }
		}
		return lo;
	}();
	static_assert(cobs::codec::size_arithmetic_fits(kLargest));
	static_assert(!cobs::codec::size_arithmetic_fits(kLargest + 1u),
	              "the guard is tight, not conservative");
	static_assert(cobs::codec::max_wire_size(kLargest) > kLargest,
	              "an accepted limit still yields a block larger than its payload");
	static_assert(cobs::codec::raw_offset(kLargest) >= 2u,
	              "and headroom at least the encoder's minimum");
	check(true, "the boundary is exact, and its geometry is still sane");

	// And the policies really do carry the assertion.
	static_assert(cobs::codec::size_arithmetic_fits(cobs::Heap<cobs::Format<64, 1024>>::Format::max_send_size));
	static_assert(cobs::codec::size_arithmetic_fits(
		cobs::Pool<cobs::Format<64, 1024>, 2, 2>::Format::max_send_size));
	check(true, "both shipped policies assert it on their own limits");
}

} // namespace

int main()
{
	group("Contract");
	runContract<cobs::Heap<cobs::Format<128, 96>>>("heap");
	runContract<cobs::Pool<cobs::Format<128, 96>, 8, 4>>("fixed");

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
