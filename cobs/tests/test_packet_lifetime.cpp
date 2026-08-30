/*
 * Host verification for the RX memory vertical: FixedPoolAllocator, RxPacket
 * and PacketRef. No decoder, no transport, no Cobs<> — if something here goes
 * red, the culprit is either the pool or the lifetime, and nothing else.
 *
 * The pool's own validation (COBS_POOL_CHECKS) is on in this build, so a
 * double free or a foreign pointer is observable as a rejection rather than as
 * a corrupted free list that surfaces three tests later.
 */
#include "FixedPoolAllocator.h"
#include "PacketRef.h"

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
using Pool = FixedPoolAllocator<128, kBlocks>;
using Packet = Pool::Packet;
using Ref = PacketRef<Pool>;

// A packet carrying recognisable contents, so a use-after-free or a mixed-up
// block shows as wrong data and not merely as a wrong count.
Packet* make(Pool& pool, const uint8_t tag, const std::size_t n)
{
	Packet* const p = pool.allocate();
	if (p == nullptr) {
		return nullptr;
	}
	const auto out = p->writable_payload();
	for (std::size_t i = 0; i < n && i < out.size(); ++i) {
		out[i] = static_cast<uint8_t>(tag + i);
	}
	p->size = static_cast<uint16_t>(n);
	return p;
}

bool contentsMatch(const Ref& r, const uint8_t tag, const std::size_t n)
{
	if (r.size() != n) {
		return false;
	}
	const auto d = r.data();
	for (std::size_t i = 0; i < n; ++i) {
		if (d[i] != static_cast<uint8_t>(tag + i)) {
			return false;
		}
	}
	return true;
}

/* ============================== the pool ================================ */

void testPoolGeometry()
{
	check(Pool::payload_capacity == 128u - sizeof(Packet),
	      "payload_capacity is the block minus the header, derived not declared");
	check(Pool::payload_capacity > 0u, "and there is payload room left");

	Pool pool;
	Packet* const p = pool.allocate();
	check(p != nullptr, "a fresh pool allocates");
	check(p->writable_payload().size() == Pool::payload_capacity,
	      "the packet's writable payload spans exactly that capacity");

	// The payload must live inside the block, right after the header.
	const auto* const header = reinterpret_cast<const unsigned char*>(p);
	const auto* const payload = p->writable_payload().data();
	check(payload == header + sizeof(Packet),
	      "the payload sits immediately after the header, in the same block");
	check(p->refs == 1 && p->size == 0 && p->next_ready == nullptr,
	      "a fresh packet carries one reference and no queue link");
	check(p->owner == &pool, "and knows which pool must reclaim it");
	pool.deallocate(p);
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

/* ============================== PacketRef =============================== */

void testRefCounting()
{
	Pool pool;
	{
		Ref a = Ref::adopt(make(pool, 0x10, 8));
		check(pool.available() == kBlocks - 1, "an adopted packet holds its block");
		check(a.get()->refs == 1, "adoption takes over the existing reference, without incrementing");
		check(contentsMatch(a, 0x10, 8), "and the payload reads back correctly");

		{
			Ref b = a;                                    // copy
			check(a.get()->refs == 2, "a copy increments the count");
			check(contentsMatch(b, 0x10, 8), "and sees the same bytes");
			Ref c = std::move(b);                         // move
			check(a.get()->refs == 2, "a move leaves the count alone");
			check(!b, "the moved-from handle is empty");  // NOLINT: intentional
			check(contentsMatch(c, 0x10, 8), "the moved-to handle owns the packet");
		}
		check(a.get()->refs == 1, "both temporaries released their references");
		check(pool.available() == kBlocks - 1, "the block is still held by the last reference");
	}
	check(pool.available() == kBlocks, "the last reference returns the block");
	check(pool.stats().rejected == 0, "and it was freed exactly once");
}

void testAssignment()
{
	Pool pool;

	Ref a = Ref::adopt(make(pool, 0x20, 4));
	Ref b = Ref::adopt(make(pool, 0x30, 6));
	check(pool.available() == kBlocks - 2, "two packets are held");

	b = a; // copy assignment: b's old packet must be released
	check(pool.available() == kBlocks - 1, "copy assignment releases the overwritten packet");
	check(a.get()->refs == 2, "and shares the assigned one");
	check(contentsMatch(b, 0x20, 4), "b now sees a's payload");

	Ref c = Ref::adopt(make(pool, 0x40, 5));
	c = std::move(a); // move assignment
	check(contentsMatch(c, 0x20, 4), "move assignment transfers the packet");
	check(!a, "leaving the source empty");
	check(pool.available() == kBlocks - 1, "and releases what c held before");

	c.reset();
	check(c.size() == 0 && !c, "reset empties the handle");
	check(pool.available() == kBlocks - 1, "b still holds the shared packet");
	b.reset();
	check(pool.available() == kBlocks, "releasing the last reference frees it");
	check(pool.stats().rejected == 0, "nothing was freed twice along the way");
}

void testSelfAssignment()
{
	Pool pool;
	Ref a = Ref::adopt(make(pool, 0x50, 3));

	// The classic way to destroy a refcounted handle: release before
	// incrementing, and self-assignment frees what it is about to keep.
	Ref& alias = a;
	a = alias;
	check(static_cast<bool>(a), "self copy-assignment leaves the handle valid");
	check(a.get()->refs == 1, "with the count unchanged");
	check(contentsMatch(a, 0x50, 3), "and the payload intact");
	check(pool.stats().rejected == 0, "nothing was freed");

	a = std::move(alias);
	check(static_cast<bool>(a), "self move-assignment leaves the handle valid");
	check(contentsMatch(a, 0x50, 3), "and the payload intact");

	a.reset();
	check(pool.available() == kBlocks, "and it still frees exactly once at the end");
}

/* ===================== ready-queue ownership transfer ==================== */

// The intrusive queue of §6.2, small enough to be obviously correct: it is
// here to exercise the ownership handshake, not to be the real one.
class ReadyQueue final {
public:
	void push(Packet* const p) noexcept // takes the caller's reference
	{
		p->next_ready = nullptr;
		if (m_tail != nullptr) { m_tail->next_ready = p; } else { m_head = p; }
		m_tail = p;
	}
	[[nodiscard]] Ref pop() noexcept // hands that reference on, unchanged
	{
		Packet* const p = m_head;
		if (p == nullptr) { return Ref{}; }
		m_head = p->next_ready;
		if (m_head == nullptr) { m_tail = nullptr; }
		p->next_ready = nullptr;
		return Ref::adopt(p);
	}
private:
	Packet* m_head = nullptr;
	Packet* m_tail = nullptr;
};

void testReadyQueueTransfer()
{
	Pool pool;
	ReadyQueue q;

	q.push(make(pool, 0x60, 2));
	q.push(make(pool, 0x70, 3));
	check(pool.available() == kBlocks - 2, "queued packets hold their blocks");

	Ref first = q.pop();
	check(contentsMatch(first, 0x60, 2), "packets come out in order");
	check(first.get()->refs == 1,
	      "the queue's reference became the handle's: no increment, no decrement");
	check(first.get()->next_ready == nullptr, "and the queue link is cleared on the way out");

	Ref second = q.pop();
	check(contentsMatch(second, 0x70, 3), "the second packet follows");
	check(!q.pop(), "an empty queue yields an empty handle");

	first.reset();
	second.reset();
	check(pool.available() == kBlocks, "draining and releasing restores the pool");
	check(pool.stats().rejected == 0, "with no double free anywhere in the handshake");
}

// The property that makes back-pressure work: an application that keeps
// packets really does consume the pool, and really does give it back.
void testRetentionConsumesCapacity()
{
	Pool pool;
	ReadyQueue q;
	std::vector<Ref> retained;

	for (std::size_t i = 0; i < kBlocks; ++i) {
		Packet* const p = make(pool, static_cast<uint8_t>(0x80 + i), 1);
		check(p != nullptr, "packet " + std::to_string(i) + " allocated");
		q.push(p);
	}
	while (Ref r = q.pop()) {
		retained.push_back(std::move(r));
	}
	check(retained.size() == kBlocks, "the application retained every packet");
	check(pool.available() == 0, "a retained packet holds pool capacity");
	check(pool.allocate() == nullptr,
	      "so the pool is genuinely exhausted while the application holds them");

	retained.clear();
	check(pool.available() == kBlocks, "releasing the retained packets restores capacity");
	Packet* const again = pool.allocate();
	check(again != nullptr, "and the pool allocates again");
	pool.deallocate(again);
	check(pool.stats().rejected == 0, "no block was ever returned twice");
}

} // namespace

int main()
{
	group("Pool");
	testPoolGeometry();
	testExhaustionAndReuse();
	testPoolRejectsAbuse();

	group("RefCounting");
	testRefCounting();
	testAssignment();
	testSelfAssignment();

	group("ReadyQueueOwnership");
	testReadyQueueTransfer();
	testRetentionConsumesCapacity();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
