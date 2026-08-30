/*
 * Host verification for the allocator and the packet geometry it lays out:
 * FixedPoolAllocator and RxPacket. No decoder, no PacketRef, no CobsRx — if
 * something here goes red, the culprit is the pool and nothing else.
 *
 * PacketRef's semantics live in test_cobs_rx.cpp instead: adopt() is private
 * to its legitimate owner, so a reference can only be obtained from a real
 * CobsRx — which is the whole point of closing that API.
 *
 * The pool's own validation (COBS_POOL_CHECKS) is on in this build, so a
 * double free or a foreign pointer is observable as a rejection rather than as
 * a corrupted free list that surfaces three tests later.
 */
#include "FixedPoolAllocator.h"

#include <cstdint>
#include <cstdio>
#include <string>
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

constexpr std::size_t kBlocks = 4;
constexpr std::size_t kCapacity = 100; // deliberately not a round block size
using Pool = FixedPoolAllocator<kCapacity, kBlocks>;
using Packet = Pool::Packet;

/* ============================== the pool ================================ */

void testPoolGeometry()
{
	// The whole point of parameterizing by capacity: this number is exactly
	// what was asked for, on every ABI. Only the RAM cost moves.
	check(Pool::payload_capacity == kCapacity,
	      "payload_capacity is exactly the requested capacity, not an ABI leftover");
	check(Pool::storage_size == sizeof(Packet) + kCapacity,
	      "storage_size is the header plus that capacity");
	check(sizeof(Pool) >= kBlocks * Pool::storage_size,
	      "and the pool is at least that big (padding may add to it)");

	Pool pool;
	Packet* const p = pool.allocate();
	check(p != nullptr, "a fresh pool allocates");
	check(p->writable_payload().size() == kCapacity,
	      "the packet's writable payload spans exactly that capacity");

	// The payload must live inside the block, right after the header.
	const auto* const header = reinterpret_cast<const unsigned char*>(p);
	const auto* const payload = p->writable_payload().data();
	check(payload == header + sizeof(Packet),
	      "the payload sits immediately after the header, in the same block");
	check(reinterpret_cast<std::uintptr_t>(p) % alignof(Packet) == 0,
	      "the packet handed out is correctly aligned");
	check(p->refs == 1 && p->size == 0 && p->next_ready == nullptr,
	      "a fresh packet carries one reference and no queue link");
	check(p->owner == &pool, "and knows which pool must reclaim it");

	// Writing the whole payload must stay inside the block: with the
	// sanitizer on, an off-by-one in the geometry fails here and nowhere else.
	const auto out = p->writable_payload();
	for (std::size_t i = 0; i < out.size(); ++i) {
		out[i] = static_cast<uint8_t>(i);
	}
	check(out[0] == 0 && out[kCapacity - 1] == static_cast<uint8_t>(kCapacity - 1),
	      "the full capacity is writable and reads back");
	pool.deallocate(p);
}

// Every block must be aligned and non-overlapping, whatever the capacity.
// A capacity that is not a multiple of the alignment is the case that would
// expose a stride computed from storage_size instead of sizeof(Block).
template<std::size_t Cap>
bool probeGeometry()
{
	using P = FixedPoolAllocator<Cap, 3>;
	using Pk = typename P::Packet;

	P pool;
	Pk* held[3] = {};
	bool ok = (P::payload_capacity == Cap);

	for (Pk*& h : held) {
		h = pool.allocate();
		ok = ok && h != nullptr &&
		     (reinterpret_cast<std::uintptr_t>(h) % alignof(Pk) == 0) &&
		     h->writable_payload().size() == Cap;
		if (h != nullptr) {
			// Touch both ends: with the sanitizer on, a stride computed from
			// storage_size instead of sizeof(Block) shows up right here.
			const auto out = h->writable_payload();
			out.front() = 0xAA;
			out.back() = 0x55;
		}
	}
	for (int i = 0; i + 1 < 3; ++i) {
		const auto a = reinterpret_cast<std::uintptr_t>(held[i]);
		const auto b = reinterpret_cast<std::uintptr_t>(held[i + 1]);
		ok = ok && ((a < b ? b - a : a - b) >= P::storage_size);
	}
	for (Pk* const h : held) { pool.deallocate(h); }
	return ok && pool.stats().rejected == 0;
}

void testGeometryAcrossCapacities()
{
	check(probeGeometry<1>(), "capacity 1 is laid out correctly");
	check(probeGeometry<7>(), "capacity 7 (odd, unaligned) is laid out correctly");
	check(probeGeometry<64>(), "capacity 64 is laid out correctly");
	check(probeGeometry<1024>(), "capacity 1024 is laid out correctly");
}

void testExhaustionAndReuse()
{
	Pool pool;
	std::vector<Packet*> held;

	for (std::size_t i = 0; i < kBlocks; ++i) {
		Packet* const p = pool.allocate();
		check(p != nullptr, "block " + std::to_string(i) + " allocates");
		held.push_back(p);
	}
	check(pool.available() == 0, "the pool is now empty");
	check(pool.allocate() == nullptr, "allocating from a dry pool returns nullptr");
	check(pool.stats().exhausted == 1, "and the exhaustion is counted");

	// Every block must be a distinct address: a pool that hands the same
	// block out twice would be the worst possible bug here.
	bool distinct = true;
	for (std::size_t i = 0; i < held.size(); ++i) {
		for (std::size_t j = i + 1; j < held.size(); ++j) {
			distinct = distinct && (held[i] != held[j]);
		}
	}
	check(distinct, "every block handed out is a distinct address");

	for (Packet* const p : held) {
		pool.deallocate(p);
	}
	check(pool.available() == kBlocks, "freeing them all restores the pool");
	check(pool.stats().in_use == 0, "and nothing is recorded as in use");

	// A second full cycle proves the free list was rebuilt correctly, not
	// merely emptied.
	for (std::size_t i = 0; i < kBlocks; ++i) {
		check(pool.allocate() != nullptr, "reuse cycle: block " + std::to_string(i));
	}
	check(pool.stats().high_water == kBlocks, "the high-water mark is the whole pool");
}

void testPoolRejectsAbuse()
{
	Pool pool;
	Packet* const p = pool.allocate();
	pool.deallocate(p);
	check(pool.available() == kBlocks, "a normal free returns the block");

	pool.deallocate(p);
	check(pool.stats().rejected == 1, "a double free is rejected, not honoured");
	check(pool.available() == kBlocks,
	      "and the pool is not corrupted into holding the block twice");

	// A pointer from another pool must not be accepted by this one.
	Pool other;
	Packet* const foreign = other.allocate();
	pool.deallocate(foreign);
	check(pool.stats().rejected == 2, "a foreign pointer is rejected");
	check(other.stats().in_use == 1, "and the block still belongs to its own pool");
	other.deallocate(foreign);

	pool.deallocate(nullptr);
	check(pool.stats().rejected == 2, "a null free is a harmless no-op, not a rejection");
}

} // namespace

int main()
{
	group("Pool");
	testPoolGeometry();
	testGeometryAcrossCapacities();
	testExhaustionAndReuse();
	testPoolRejectsAbuse();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
