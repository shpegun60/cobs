/*
 * Host verification for detail::StaticBlockPool, the raw memory primitive
 * both allocator policies are built on. No packets, no decoder, no policy —
 * if something here goes red, the culprit is the block pool and nothing else.
 *
 * The policies themselves are tested in test_allocators.cpp, against the
 * contract rather than against their internals.
 *
 * The pool's own validation (COBS_POOL_CHECKS) is on in this build, so a
 * double free or a foreign pointer is observable as a rejection rather than as
 * a corrupted free list that surfaces three tests later.
 */
#include "detail/StaticBlockPool.h"

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

/* ====================== the raw block primitive ========================= */

// What the RX adapter is built on, and what TX will use directly: blocks of
// plain bytes, possibly smaller than the free-list link the pool threads
// through them.
void testRawPoolHandlesTinyBlocks()
{
	// A two-byte block with byte alignment — the shape a minimal TX frame
	// (01 00) would ask for, and one that cannot physically hold a pointer.
	using Tiny = cobs::detail::StaticBlockPool<2, 4, 1>;
	Tiny pool;

	check(Tiny::block_size == 2, "the client-visible block size is what was asked for");
	check(Tiny::storage_size >= sizeof(void*),
	      "while the pool quietly spends enough to thread its free list");

	std::byte* held[4] = {};
	bool distinct = true;
	for (int i = 0; i < 4; ++i) {
		held[i] = pool.allocate();
		check(held[i] != nullptr, "tiny block " + std::to_string(i) + " allocates");
		held[i][0] = std::byte{0x01};
		held[i][1] = std::byte{0x00};
	}
	for (int i = 0; i < 4; ++i) {
		for (int j = i + 1; j < 4; ++j) {
			distinct = distinct && held[i] != held[j];
		}
	}
	check(distinct, "and they are distinct, non-overlapping blocks");
	check(pool.allocate() == nullptr, "a dry tiny pool returns nullptr");

	for (std::byte* const p : held) { pool.deallocate(p); }
	check(pool.available() == 4, "freeing them restores the pool");
	check(pool.stats().rejected == 0, "with nothing rejected");
}

// The ordering that made release() take a callback: a destructor must never
// run on memory the pool is about to refuse.
void testCleanupRunsOnlyAfterValidation()
{
	using Small = cobs::detail::StaticBlockPool<32, 2, 1>;
	Small pool;
	Small other;

	int cleanups = 0;
	const auto count = [&cleanups](std::byte*) noexcept { ++cleanups; };

	std::byte* const good = pool.allocate();
	check(pool.release(good, count) && cleanups == 1,
	      "a valid block runs its cleanup and is returned");

	check(!pool.release(good, count) && cleanups == 1,
	      "a double free is refused BEFORE the cleanup runs");

	std::byte* const foreign = other.allocate();
	check(!pool.release(foreign, count) && cleanups == 1,
	      "a foreign pointer is refused before the cleanup runs");
	check(other.stats().in_use == 1, "and still belongs to its own pool");

	check(!pool.release(nullptr, count) && cleanups == 1,
	      "a null release is a no-op");
	check(pool.stats().rejected == 2, "exactly the two abuses were counted");
}

} // namespace

int main()
{
	group("RawBlockPool");
	testRawPoolHandlesTinyBlocks();
	testCleanupRunsOnlyAfterValidation();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
