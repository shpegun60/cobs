/*
 * The assembled engine, end to end, over a fake transport built from the same
 * delegates a real one would supply.
 *
 * Like test_allocators, the whole body runs TWICE — once over the heap policy
 * and once over the fixed one — from a single template. If Cobs ever had to
 * know which it was talking to, the abstraction would have leaked.
 */
#include "Cobs.h"
#include "CobsFixedAllocator.h"
#include "CobsHeapAllocator.h"
#include "reference_encoder.h"

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

constexpr std::size_t kPayload = 6; // a convenient non-trivial frame size

// What a UART looks like from up here: two questions and one answer.
struct FakeTransport {
	std::vector<std::vector<uint8_t>> sent;
	bool busy = false;
	bool refuse = false;      // make send() fail to start, as hardware can
	int  busy_queries = 0;    // proves the idle path never asks

	bool send(std::span<const uint8_t> frame) noexcept
	{
		if (refuse) {
			return false;
		}
		sent.emplace_back(frame.begin(), frame.end());
		busy = true;
		return true;
	}
	bool tx_busy() noexcept { ++busy_queries; return busy; }
	void finish() noexcept { busy = false; } // the transport let the block go
};

std::vector<uint8_t> pattern(const uint8_t tag, const std::size_t n)
{
	std::vector<uint8_t> v(n);
	for (std::size_t i = 0; i < n; ++i) { v[i] = static_cast<uint8_t>(tag + i); }
	return v;
}

template<class C>
typename C::Sender sender_for(FakeTransport& t)
{
	return typename C::Sender{
		[&t](std::span<const uint8_t> f) noexcept { return t.send(f); }};
}
template<class C>
typename C::TxBusy busy_for(FakeTransport& t)
{
	return typename C::TxBusy{[&t]() noexcept { return t.tx_busy(); }};
}

template<class C>
void bind(C& cobs, FakeTransport& t)
{
	check(cobs.set_transport(sender_for<C>(t), busy_for<C>(t)), "the transport binds");
}

/* ==================== the engine, for either policy ===================== */

template<class Allocator>
void runEngine(const char* name)
{
	g_policy = name;
	using Engine = Cobs<Allocator>;

	static_assert(!std::is_copy_constructible_v<Engine>, "Cobs must not be copyable");
	static_assert(!std::is_move_constructible_v<Engine>,
	              "Cobs must not be movable: PacketRef points into it");

	/* --- RX: bytes in, packets out ------------------------------------- */
	{
		Engine cobs;
		const auto a = pattern(0x10, 5);
		const auto b = pattern(0x50, 9);
		std::vector<uint8_t> wire;
		for (const auto* p : {&a, &b}) {
			for (const uint8_t x : cobs_test::encode(*p)) { wire.push_back(x); }
		}
		cobs.consume(std::span<const uint8_t>{wire});

		check(cobs.rx_stats().frames_delivered == 2, "two frames arrive through the engine");
		const auto r1 = cobs.pop_packet();
		const auto r2 = cobs.pop_packet();
		check(r1.size() == a.size() && r2.size() == b.size(),
		      "and pop_packet hands them over in order");
		check(!cobs.pop_packet(), "then the queue is empty");
	}

	/* --- TX: the happy path -------------------------------------------- */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);

		auto msg = cobs.make_msg();
		check(static_cast<bool>(msg), "make_msg yields an empty message");
		check(msg.size() == 0, "with nothing in it yet");
		check(msg.capacity() >= Engine::default_capacity_hint,
		      "but with the default reserve already in hand");
		const auto payload = pattern(0x20, 6);
		check(msg.write_bytes(std::span<const uint8_t>{payload}), "the payload is written");

		check(cobs.push(msg) == SendResult::Sent, "push sends it");
		check(!msg, "the message surrendered its block");
		check(cobs.tx_active(), "which the engine now holds");
		check(t.sent.size() == 1 && t.sent[0] == cobs_test::encode(payload),
		      "and the transport received exactly the canonical frame");

		// proceed() must not free the block while the transport still reads it.
		cobs.proceed();
		check(cobs.tx_active(), "proceed does not reclaim it while the transport is busy");
		t.finish();
		cobs.proceed();
		check(!cobs.tx_active(), "and reclaims it once the transport lets go");
	}

	/* --- the idle path never asks the transport ------------------------ */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);
		const int before = t.busy_queries;
		for (int i = 0; i < 100; ++i) { cobs.proceed(); }
		check(t.busy_queries == before,
		      "100 idle proceed() calls never invoke tx_busy (the null check wins)");
	}

	/* --- Busy leaves the message Building and writable ------------------ */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);

		auto first = cobs.make_msg();
		check(first.write(uint8_t{0x01}), "a one-byte frame is built");
		check(cobs.push(first) == SendResult::Sent, "the first frame goes out");

		auto second = cobs.make_msg(4);
		const auto payload = pattern(0x30, 4);
		check(second.write_bytes(std::span<const uint8_t>{payload}), "and a second built");

		check(cobs.push(second) == SendResult::Busy, "a second push while busy is refused");
		check(static_cast<bool>(second) && !second.encoded(),
		      "leaving the message Building, not encoded");
		check(cobs.tx_stats().send_refused_busy == 1, "and counted");

		// Still Building, so it is still a builder: a refused push touched
		// nothing, and more bytes may be appended before the retry.
		check(second.size() == payload.size(), "its payload is untouched");
		check(second.write(uint8_t{0x34}), "and it still accepts writes");
		check(second.size() == payload.size() + 1, "which land after what was there");

		t.finish();
		cobs.proceed();
		check(cobs.push(second) == SendResult::Sent, "it sends once the link frees up");
		auto expected = payload;
		expected.push_back(0x34);
		check(t.sent.back() == cobs_test::encode(expected),
		      "carrying everything that was written, in order");
	}

	/* --- a failed start keeps the SAME frame retryable ------------------ */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);

		auto msg = cobs.make_msg(7);
		const auto payload = pattern(0x40, 7);
		check(msg.write_bytes(std::span<const uint8_t>{payload}), "a frame is built");

		t.refuse = true;
		check(cobs.push(msg) == SendResult::Error, "a transport that will not start reports Error");
		check(static_cast<bool>(msg) && msg.encoded(),
		      "the message survives, already encoded");
		check(!cobs.tx_active(), "and the engine took no ownership");
		check(cobs.tx_stats().send_failed == 1, "the failure is counted");

		t.refuse = false;
		check(cobs.push(msg) == SendResult::Sent, "the retry succeeds");
		check(t.sent.size() == 1 && t.sent[0] == cobs_test::encode(payload),
		      "sending the identical frame, encoded exactly once");
	}

	/* --- refusals that are not failures --------------------------------- */
	{
		Engine cobs;                       // nothing bound
		auto msg = cobs.make_msg(kPayload);
		
		check(cobs.push(msg) == SendResult::NotBound, "pushing with no transport bound is NotBound");
		check(static_cast<bool>(msg), "and the message is untouched");

		typename Engine::Msg empty;
		check(cobs.push(empty) == SendResult::Invalid, "an empty message is Invalid");

		Engine other;
		auto foreign = other.make_msg(2);
		
		check(cobs.push(foreign) == SendResult::Invalid,
		      "so is a message belonging to another engine of the same type");
	}

	/* --- binding is atomic, and refused mid-transfer -------------------- */
	{
		Engine cobs;
		FakeTransport t;
		FakeTransport other;

		// Half a transport is refused: this is the state that made a mixed
		// pair (sender on one link, tx_busy on another) reachable at all.
		check(!cobs.set_transport(sender_for<Engine>(t), typename Engine::TxBusy{}),
		      "a sender with no tx_busy is refused");
		check(!cobs.set_transport(typename Engine::Sender{}, busy_for<Engine>(t)),
		      "and a tx_busy with no sender likewise");
		{
			auto msg = cobs.make_msg(kPayload);
			
			check(cobs.push(msg) == SendResult::NotBound,
			      "so neither half leaked into the engine");
		}

		bind(cobs, t);
		auto msg = cobs.make_msg(kPayload);
		
		check(cobs.push(msg) == SendResult::Sent, "a frame is in flight");

		check(!cobs.set_transport(sender_for<Engine>(other), busy_for<Engine>(other)),
		      "the transport cannot be swapped under a live transfer — a new "
		      "tx_busy saying 'idle' would free the block under the DMA");

		t.finish();
		cobs.proceed();
		check(cobs.set_transport(sender_for<Engine>(other), busy_for<Engine>(other)),
		      "and may be rebound once the link is idle");
		check(cobs.set_transport(typename Engine::Sender{}, typename Engine::TxBusy{}),
		      "two empty delegates are a clean unbind");
	}

	/* --- full duplex over one engine ------------------------------------ */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);

		const auto out = pattern(0x60, 5);
		auto msg = cobs.make_msg();
		check(msg.write_bytes(std::span<const uint8_t>{out}), "a frame is built");
		check(cobs.push(msg) == SendResult::Sent, "a frame goes out");

		const auto in = pattern(0x70, 11);
		cobs.consume(std::span<const uint8_t>{cobs_test::encode(in)});
		const auto got = cobs.pop_packet();
		check(got.size() == in.size(), "while a frame comes in on the same engine");

		t.finish();
		cobs.proceed();
		check(!cobs.tx_active() && cobs.rx_stats().frames_delivered == 1,
		      "and both directions settle independently");
	}
}

/* ==================== policy-specific: exhaustion ======================= */

void testFixedExhaustion()
{
	g_policy = "fixed";
	using Allocator = CobsFixedAllocator<32, 2, 32, 1>;
	Cobs<Allocator> cobs;
	FakeTransport t;
	bind(cobs, t);

	auto a = cobs.make_msg(4);
	check(static_cast<bool>(a), "the single TX block is handed out");
	auto b = cobs.make_msg(4);
	check(!b, "a second message from a one-block pool is empty");
	check(cobs.push(b) == SendResult::Invalid, "and pushing it is Invalid, not a crash");

	
	check(cobs.push(a) == SendResult::Sent, "the real one still sends");

	// The block is with the transport, so the pool is dry until it returns.
	auto c = cobs.make_msg(4);
	check(!c, "the pool stays dry while the transport holds the block");
	t.finish();
	cobs.proceed();
	auto d = cobs.make_msg(4);
	check(static_cast<bool>(d), "and refills once proceed reclaims it");
}

// The destructor must return an in-flight block, or a pool-backed engine would
// look leaked to anything watching its occupancy.
void testDestructorReclaimsActiveTx()
{
	g_policy = "fixed";
	using Allocator = CobsFixedAllocator<32, 2, 32, 2>;
	FakeTransport t;
	{
		Cobs<Allocator> cobs;
		bind(cobs, t);
		auto msg = cobs.make_msg();
		check(msg.write(uint32_t{0x01020304u}), "a frame is built");
		check(cobs.push(msg) == SendResult::Sent, "a frame is in flight at destruction");
		check(cobs.allocator().tx_available() == 1, "one TX block is out");
		// The transport is finished with it — precondition 2 is satisfied.
		t.finish();
	}
	check(true, "destroying the engine with a reclaimed block is clean");
}


/* ==================== the default reserve, and its clamp ================ */

void testDefaultCapacityHint()
{
	{	// The heap policy grants exactly what is asked, so the default is
		// visible directly: a short frame never reallocates.
		g_policy = "heap";
		using Engine = Cobs<CobsHeapAllocator<64, 1024>>;
		static_assert(Engine::default_capacity_hint == 32);
		Engine cobs;

		auto msg = cobs.make_msg();
		check(msg.size() == 0 && msg.capacity() == 32,
		      "make_msg() reserves default_capacity_hint");

		bool ok = true;
		for (int i = 0; i < 32; ++i) { ok = ok && msg.write(static_cast<uint8_t>(i)); }
		check(ok && msg.capacity() == 32,
		      "and 32 bytes go in without a single reallocation");

		// The explicit minimum is still reachable, for a caller that wants the
		// canonical empty frame and nothing more.
		auto minimal = cobs.make_msg(0);
		check(static_cast<bool>(minimal) && minimal.capacity() == 0,
		      "make_msg(0) is still explicitly minimal");
		check(!minimal.encode().empty(), "and encodes the canonical empty frame");
	}
	{	// A policy whose limit is BELOW the default must still work: the
		// default is clamped, so make_msg() can never fail on its own default.
		g_policy = "fixed";
		using Engine = Cobs<CobsFixedAllocator<32, 2, 16, 1>>;
		static_assert(Engine::default_capacity_hint == 16,
		              "the default must clamp to a smaller tx_max_size");
		Engine cobs;
		auto msg = cobs.make_msg();
		check(static_cast<bool>(msg),
		      "make_msg() works on a policy whose limit is below the default");
		check(msg.capacity() == 16, "reporting the slab it actually got");
	}
}


/*
 * The reported capacity has to survive the WHOLE ownership chain, not just
 * CobsMsg:
 *
 *     CobsMsg -> surrender_block() -> Cobs::m_activeTx -> proceed()
 *             -> deallocate_tx(memory, reported_capacity)
 *
 * Neither shipped policy can catch a regression here. The heap one reports
 * capacity == requested, so a mix-up is invisible; the fixed one ignores the
 * capacity at free time entirely. So this uses a policy that over-allocates
 * the way a segregated allocator would, and checks the number that comes back
 * at the far end of the chain.
 */
class OverallocPolicy final {
public:
	static constexpr std::size_t rx_max_size = 64;
	static constexpr std::size_t tx_max_size = 512;

	using Packet = RxPacket<OverallocPolicy>;
	[[nodiscard]] Packet* allocate_rx() noexcept
	{
		void* const memory = ::operator new(sizeof(Packet) + rx_max_size, std::nothrow);
		if (memory == nullptr) { return nullptr; }
		Packet* const p = std::construct_at(static_cast<Packet*>(memory));
		p->owner = this;
		return p;
	}
	void deallocate_rx(Packet* const p) noexcept
	{
		if (p == nullptr) { return; }
		std::destroy_at(p);
		::operator delete(static_cast<void*>(p));
	}

	[[nodiscard]] TxAllocation allocate_tx(const std::size_t requested) noexcept
	{
		if (requested > tx_max_size) { return {}; }
		const std::size_t doubled = requested * 2u + 1u;
		const std::size_t capacity = (doubled > tx_max_size) ? tx_max_size : doubled;
		void* const memory = ::operator new(cobs_max_wire_size(capacity), std::nothrow);
		if (memory == nullptr) { return {}; }
		++allocations;
		last_granted = capacity;
		return {static_cast<std::byte*>(memory), capacity};
	}
	void deallocate_tx(std::byte* const memory, const std::size_t capacity) noexcept
	{
		if (memory != nullptr) { ++frees; last_freed = capacity; }
		::operator delete(static_cast<void*>(memory));
	}

	std::size_t allocations = 0;
	std::size_t frees = 0;
	std::size_t last_granted = 0;
	std::size_t last_freed = 0;
};

void testReportedCapacitySurvivesTheEngine()
{
	g_policy = "overalloc";
	using Engine = Cobs<OverallocPolicy>;
	Engine cobs;
	FakeTransport t;
	bind(cobs, t);

	auto msg = cobs.make_msg(10);
	check(static_cast<bool>(msg) && msg.capacity() == 21,
	      "the policy grants more than was asked for (10 -> 21)");

	const auto payload = pattern(0x80, 12);
	check(msg.write_bytes(std::span<const uint8_t>{payload}), "a payload is written");
	check(cobs.push(msg) == SendResult::Sent, "and pushed");
	check(t.sent.size() == 1 && t.sent[0] == cobs_test::encode(payload),
	      "the transport got the canonical frame");
	check(cobs.allocator().frees == 0, "nothing is freed while the transport reads");

	t.finish();
	cobs.proceed();
	check(cobs.allocator().frees == 1, "proceed reclaims the block");
	check(cobs.allocator().last_freed == 21,
	      "returning it with the capacity the POLICY reported, not the 10 requested "
	      "nor the 12 written");

	{	// The same, after a growth: the capacity that travels to activeTx must
		// be the CURRENT block's, not the one the message was born with.
		auto grown = cobs.make_msg(4);
		check(grown.capacity() == 9, "a second message starts at 9");
		const auto big = pattern(0x10, 60);
		check(grown.write_bytes(std::span<const uint8_t>{big}), "60 bytes force a growth");
		const std::size_t after_growth = grown.capacity();
		check(after_growth == 121, "to 121 (asked 60, granted 121)");
		check(cobs.push(grown) == SendResult::Sent, "it sends");
		t.finish();
		cobs.proceed();
		check(cobs.allocator().last_freed == after_growth,
		      "and comes back with the GROWN capacity, not the original 9");
	}
}

} // namespace

int main()
{
	group("Engine");
	runEngine<CobsHeapAllocator<64, 64>>("heap");
	runEngine<CobsFixedAllocator<64, 4, 64, 2>>("fixed");

	group("FixedSpecific");
	testFixedExhaustion();
	testDefaultCapacityHint();
	testReportedCapacitySurvivesTheEngine();
	testDestructorReclaimsActiveTx();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
