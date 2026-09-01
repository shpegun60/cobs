/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * The assembled engine, end to end, over a fake transport built from the same
 * delegates a real one would supply.
 *
 * Like test_storage, the whole body runs TWICE — once over the heap strategy
 * and once over the fixed one — from a single template. If Endpoint ever had to
 * know which it was talking to, the abstraction would have leaked.
 */
#include "Cobs.h"
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
typename C::BusyQuery busy_for(FakeTransport& t)
{
	return typename C::BusyQuery{[&t]() noexcept { return t.tx_busy(); }};
}

template<class C>
void bind(C& cobs, FakeTransport& t)
{
	check(cobs.bind(sender_for<C>(t), busy_for<C>(t)), "the transport binds");
}

/* =================== the engine, for either storage ===================== */

template<class StorageT>
void runEngine(const char* name)
{
	g_strategy = name;
	using Engine = cobs::Endpoint<StorageT>;

	static_assert(!std::is_copy_constructible_v<Engine>, "Endpoint must not be copyable");
	static_assert(!std::is_move_constructible_v<Engine>,
	              "Endpoint must not be movable: cobs::Packet points into it");
	static_assert(std::is_same_v<decltype(std::declval<const Engine&>().stats()), cobs::Stats>,
	              "Endpoint observation is one value snapshot");
	static_assert(!requires(const Engine& engine) { engine.rx_stats(); });
	static_assert(!requires(const Engine& engine) { engine.tx_stats(); });

	/* --- RX: bytes in, packets out ------------------------------------- */
	{
		Engine cobs;
		const auto a = pattern(0x10, 5);
		const auto b = pattern(0x50, 9);
		std::vector<uint8_t> wire;
		for (const auto* p : {&a, &b}) {
			for (const uint8_t x : cobs_test::frame(*p, Engine::length_size)) { wire.push_back(x); }
		}
		cobs.consume(std::span<const uint8_t>{wire});

		auto stats = cobs.stats();
		check(stats.rx.frames_delivered == 2, "two frames arrive through the engine");
		stats.rx.frames_delivered = 99;
		check(cobs.stats().rx.frames_delivered == 2,
		      "the Stats value is a snapshot, not mutable engine state");
		const auto r1 = cobs.pop_packet();
		const auto r2 = cobs.pop_packet();
		check(r1.size() == a.size() && r2.size() == b.size(),
		      "and pop_packet hands them over in order");
		check(!cobs.pop_packet(), "then the queue is empty");
	}
	{
		Engine cobs;
		cobs.notify_gap();
		const auto stats = cobs.stats();
		check(stats.rx.frames_lost == 1 && stats.rx.resyncs == 1,
		      "notify_gap forwards transport discontinuity to the receiver");
	}

	/* --- TX: the happy path -------------------------------------------- */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);

		auto msg = cobs.make_message();
		check(static_cast<bool>(msg), "make_message yields an empty message");
		check(msg.size() == 0, "with nothing in it yet");
		check(msg.capacity() >= Engine::default_capacity_hint,
		      "but with the default reserve already in hand");
		const auto payload = pattern(0x20, 6);
		check(msg.append_bytes(std::span<const uint8_t>{payload}), "the payload is appended");

		check(cobs.send(msg) == cobs::SendResult::Sent, "send starts the transfer");
		check(!msg, "the message surrendered its block");
		check(cobs.tx_active(), "which the engine now holds");
		check(t.sent.size() == 1 && t.sent[0] == cobs_test::frame(payload, Engine::length_size),
		      "and the transport received exactly the canonical frame");

		// poll() must not free the block while the transport still reads it.
		cobs.poll();
		check(cobs.tx_active(), "poll does not reclaim it while the transport is busy");
		t.finish();
		cobs.poll();
		check(!cobs.tx_active(), "and reclaims it once the transport lets go");
	}

	/* --- the idle path never asks the transport ------------------------ */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);
		const int before = t.busy_queries;
		for (int i = 0; i < 100; ++i) { cobs.poll(); }
		check(t.busy_queries == before,
		      "100 idle poll() calls never invoke the busy query (the null check wins)");
	}

	/* --- Busy leaves the message Building and writable ------------------ */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);

		auto first = cobs.make_message();
		check(first.append_native(uint8_t{0x01}), "a one-byte frame is built");
		check(cobs.send(first) == cobs::SendResult::Sent, "the first frame goes out");

		auto second = cobs.make_message(4);
		const auto payload = pattern(0x30, 4);
		check(second.append_bytes(std::span<const uint8_t>{payload}), "and a second built");

		check(cobs.send(second) == cobs::SendResult::Busy, "a second send while busy is refused");
		check(static_cast<bool>(second),
		      "leaving the message owned by the caller");
		check(cobs.stats().tx.send_refused_busy == 1, "and counted");

		// Still Building, so it is still a builder: a refused send touched
		// nothing, and more bytes may be appended before the retry.
		check(second.size() == payload.size(), "its payload is untouched");
		check(second.append_native(uint8_t{0x34}), "and it still accepts appends");
		check(second.size() == payload.size() + 1, "which land after what was there");

		t.finish();
		cobs.poll();
		check(cobs.send(second) == cobs::SendResult::Sent, "it sends once the link frees up");
		auto expected = payload;
		expected.push_back(0x34);
		check(t.sent.back() == cobs_test::frame(expected, Engine::length_size),
		      "carrying everything that was appended, in order");
	}

	/* --- a failed start keeps the SAME frame retryable ------------------ */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);

		auto msg = cobs.make_message(7);
		const auto payload = pattern(0x40, 7);
		check(msg.append_bytes(std::span<const uint8_t>{payload}), "a frame is built");

		t.refuse = true;
		check(cobs.send(msg) == cobs::SendResult::Failed,
		      "a transport that will not start reports Failed");
		check(static_cast<bool>(msg) && msg.size() == payload.size(),
		      "the message survives with its payload identity intact");
		check(!msg.append_native(uint8_t{0xFF}) && !msg.reserve(msg.capacity()),
		      "its coordinator-owned encoded state refuses further building");
		check(!cobs.tx_active(), "and the engine took no ownership");
		check(cobs.stats().tx.send_failed == 1, "the failure is counted");

		t.refuse = false;
		check(cobs.send(msg) == cobs::SendResult::Sent, "the retry succeeds");
		check(t.sent.size() == 1 && t.sent[0] == cobs_test::frame(payload, Engine::length_size),
		      "sending the identical frame, encoded exactly once");
	}

	/* --- refusals that are not failures --------------------------------- */
	{
		Engine cobs;                       // nothing bound
		auto msg = cobs.make_message(kPayload);
		
		check(cobs.send(msg) == cobs::SendResult::Unbound,
		      "sending with no transport bound reports Unbound");
		check(static_cast<bool>(msg), "and the message is untouched");

		typename Engine::Message empty;
		check(cobs.send(empty) == cobs::SendResult::Invalid, "an empty message is Invalid");

		Engine other;
		auto foreign = other.make_message(2);
		
		check(cobs.send(foreign) == cobs::SendResult::Invalid,
		      "so is a message belonging to another engine of the same type");
	}

	/* --- binding is atomic, and refused mid-transfer -------------------- */
	{
		Engine cobs;
		FakeTransport t;
		FakeTransport other;

		// Half a transport is refused: this is the state that made a mixed
		// pair (sender on one link, tx_busy on another) reachable at all.
		check(!cobs.bind(sender_for<Engine>(t), typename Engine::BusyQuery{}),
		      "a sender with no tx_busy is refused");
		check(!cobs.bind(typename Engine::Sender{}, busy_for<Engine>(t)),
		      "and a tx_busy with no sender likewise");
		check(!cobs.bind(typename Engine::Sender{}, typename Engine::BusyQuery{}),
		      "bind also refuses two empty delegates; unbind is explicit");
		{
			auto msg = cobs.make_message(kPayload);
			
			check(cobs.send(msg) == cobs::SendResult::Unbound,
			      "so neither half leaked into the engine");
		}

		bind(cobs, t);
		check(!cobs.bind(sender_for<Engine>(other), typename Engine::BusyQuery{}),
		      "a failed partial rebind leaves the established pair untouched");
		auto msg = cobs.make_message(kPayload);

		check(cobs.send(msg) == cobs::SendResult::Sent, "a frame is in flight");
		check(t.sent.size() == 1 && other.sent.empty(),
		      "the original sender and busy query still act as one pair");

		check(!cobs.bind(sender_for<Engine>(other), busy_for<Engine>(other)),
		      "the transport cannot be swapped under a live transfer — a new "
		      "tx_busy saying 'idle' would free the block under the DMA");
		check(!cobs.unbind(), "and cannot be unbound while that transfer is live");

		t.finish();
		cobs.poll();
		check(cobs.bind(sender_for<Engine>(other), busy_for<Engine>(other)),
		      "and may be rebound once the link is idle");
		check(cobs.unbind(), "explicit unbind succeeds once the link is idle");
		auto after_unbind = cobs.make_message(1);
		check(cobs.send(after_unbind) == cobs::SendResult::Unbound,
		      "and subsequent sends report Unbound");
	}

	/* --- full duplex over one engine ------------------------------------ */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);

		const auto out = pattern(0x60, 5);
		auto msg = cobs.make_message();
		check(msg.append_bytes(std::span<const uint8_t>{out}), "a frame is built");
		check(cobs.send(msg) == cobs::SendResult::Sent, "a frame goes out");

		const auto in = pattern(0x70, 11);
		cobs.consume(std::span<const uint8_t>{cobs_test::frame(in, Engine::length_size)});
		const auto got = cobs.pop_packet();
		check(got.size() == in.size(), "while a frame comes in on the same engine");

		t.finish();
		cobs.poll();
		const auto stats = cobs.stats();
		check(!cobs.tx_active() && stats.rx.frames_delivered == 1 &&
		          stats.tx.frames_sent == 1,
		      "and one snapshot reports both directions settled independently");
	}
}

/* ==================== policy-specific: exhaustion ======================= */

void testFixedExhaustion()
{
	g_strategy = "fixed";
	using Memory = cobs::Pool<2, 1, cobs::Format<32>>;
	cobs::Endpoint<Memory> cobs;
	FakeTransport t;
	bind(cobs, t);

	auto a = cobs.make_message(4);
	check(static_cast<bool>(a), "the single TX block is handed out");
	auto b = cobs.make_message(4);
	check(!b, "a second message from a one-block pool is empty");
		check(cobs.send(b) == cobs::SendResult::Invalid,
		      "and sending it reports Invalid rather than crashing");

	
	check(cobs.send(a) == cobs::SendResult::Sent, "the real one still sends");

	// The block is with the transport, so the pool is dry until it returns.
	auto c = cobs.make_message(4);
	check(!c, "the pool stays dry while the transport holds the block");
	t.finish();
	cobs.poll();
	auto d = cobs.make_message(4);
	check(static_cast<bool>(d), "and refills once poll reclaims it");
}

// The destructor must return an in-flight block, or a pool-backed engine would
// look leaked to anything watching its occupancy.
void testDestructorReclaimsActiveTx()
{
	g_strategy = "fixed";
	using Memory = cobs::Pool<2, 2, cobs::Format<32>>;
	FakeTransport t;
	{
		cobs::Endpoint<Memory> cobs;
		bind(cobs, t);
		auto msg = cobs.make_message();
		check(msg.append_native(uint32_t{0x01020304u}), "a frame is built");
		check(cobs.send(msg) == cobs::SendResult::Sent, "a frame is in flight at destruction");
		check(cobs.storage().tx_available() == 1, "one TX block is out");
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
		g_strategy = "heap";
		using Engine = cobs::Endpoint<cobs::Heap<cobs::Format<64, 1024>>>;
		static_assert(Engine::default_capacity_hint == 32);
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);

		auto msg = cobs.make_message();
		check(msg.size() == 0 && msg.capacity() == 32,
		      "make_message() reserves default_capacity_hint");

		bool ok = true;
		for (int i = 0; i < 32; ++i) { ok = ok && msg.append_native(static_cast<uint8_t>(i)); }
		check(ok && msg.capacity() == 32,
		      "and 32 bytes go in without a single reallocation");

		// make_message(0) is a zero capacity REQUEST, not an empty-only message.
		// Sent straight away it is the canonical empty frame; appended to,
		// it grows like anything else.
		auto minimal = cobs.make_message(0);
		check(static_cast<bool>(minimal) && minimal.capacity() == 0,
		      "make_message(0) reserves nothing");
		{
			auto empty_frame = cobs.make_message(0);
			check(cobs.send(empty_frame) == cobs::SendResult::Sent &&
			      t.sent.back() == cobs_test::frame({}, Engine::length_size),
			      "and sent straight away it is the canonical empty frame");
			t.finish();
			cobs.poll();
		}
		check(minimal.append_native(uint8_t{0x42}), "but it still accepts an append");
		check(minimal.size() == 1 && minimal.capacity() >= 1,
		      "growing from zero capacity like any other message");
		const std::vector<uint8_t> more(40, 0x5A);
		check(minimal.append_bytes(std::span<const uint8_t>{more}), "and keeps growing");
		check(minimal.size() == 41, "to whatever it is given");
	}
	{	// A policy whose limit is BELOW the default must still work: the
		// default is clamped, so make_message() can never fail on its own default.
		g_strategy = "fixed";
		using Engine = cobs::Endpoint<cobs::Pool<2, 1, cobs::Format<32, 16>>>;
		static_assert(Engine::default_capacity_hint == 16,
		              "the default must clamp to a smaller tx_max_size");
		Engine cobs;
		auto msg = cobs.make_message();
		check(static_cast<bool>(msg),
		      "make_message() works on a policy whose limit is below the default");
		check(msg.capacity() == 16, "reporting the slab it actually got");
	}
}


/*
 * The reported capacity has to survive the WHOLE ownership chain, not just
 * cobs::Message:
 *
 *     cobs::Message -> surrender_block() -> Endpoint::m_activeTx -> poll()
 *             -> release_tx(the same TxBlock descriptor)
 *
 * Neither shipped policy can catch a regression here. The heap one reports
 * capacity == requested, so a mix-up is invisible; the fixed one ignores the
 * capacity at free time entirely. So this uses a policy that over-allocates
 * the way segregated storage would, and checks the number that comes back
 * at the far end of the chain.
 */
class OvergrantStorage final {
public:
	using Format = cobs::Format<64, 512>;
	using RxBlock = cobs::RxBlock<OvergrantStorage>;

	[[nodiscard]] RxBlock* acquire_rx(const std::size_t requested_size) noexcept
	{
		if (requested_size > Format::max_receive_size) { return nullptr; }
		void* const memory = ::operator new(sizeof(RxBlock) + requested_size, std::nothrow);
		if (memory == nullptr) { return nullptr; }
		return std::construct_at(static_cast<RxBlock*>(memory));
	}
	void release_rx(RxBlock* const block) noexcept
	{
		if (block == nullptr) { return; }
		std::destroy_at(block);
		::operator delete(static_cast<void*>(block));
	}

	[[nodiscard]] cobs::TxBlock acquire_tx(const std::size_t requested) noexcept
	{
		if (requested > Format::max_send_size) { return {}; }
		const std::size_t doubled = requested * 2u + 1u;
		const std::size_t capacity =
			(doubled > Format::max_send_size) ? Format::max_send_size : doubled;
		void* const memory = ::operator new(Format::tx_storage_size_for_capacity(capacity), std::nothrow);
		if (memory == nullptr) { return {}; }
		++allocations;
		last_granted = capacity;
		return {static_cast<std::byte*>(memory), capacity};
	}
	void release_tx(const cobs::TxBlock block) noexcept
	{
		if (block.memory != nullptr) { ++frees; last_freed = block.capacity; }
		::operator delete(static_cast<void*>(block.memory));
	}

	std::size_t allocations = 0;
	std::size_t frees = 0;
	std::size_t last_granted = 0;
	std::size_t last_freed = 0;
};

void testReportedCapacitySurvivesTheEngine()
{
	g_strategy = "overalloc";
	using Engine = cobs::Endpoint<OvergrantStorage>;
	Engine cobs;
	FakeTransport t;
	bind(cobs, t);

	auto msg = cobs.make_message(10);
	check(static_cast<bool>(msg) && msg.capacity() == 21,
	      "the policy grants more than was asked for (10 -> 21)");

	const auto payload = pattern(0x80, 12);
	check(msg.append_bytes(std::span<const uint8_t>{payload}), "a payload is appended");
	check(cobs.send(msg) == cobs::SendResult::Sent, "and sent");
	check(t.sent.size() == 1 && t.sent[0] == cobs_test::frame(payload, Engine::length_size),
	      "the transport got the canonical frame");
	check(cobs.storage().frees == 0, "nothing is freed while the transport reads");

	t.finish();
	cobs.poll();
	check(cobs.storage().frees == 1, "poll reclaims the block");
	check(cobs.storage().last_freed == 21,
	      "returning it with the capacity the POLICY reported, not the 10 requested "
	      "nor the 12 appended");

	{	// The same, after a growth: the capacity that travels to activeTx must
		// be the CURRENT block's, not the one the message was born with.
		auto grown = cobs.make_message(4);
		check(grown.capacity() == 9, "a second message starts at 9");
		const auto big = pattern(0x10, 60);
		check(grown.append_bytes(std::span<const uint8_t>{big}), "60 bytes force a growth");
		const std::size_t after_growth = grown.capacity();
		check(after_growth == 121, "to 121 (asked 60, granted 121)");
		check(cobs.send(grown) == cobs::SendResult::Sent, "it sends");
		t.finish();
		cobs.poll();
		check(cobs.storage().last_freed == after_growth,
		      "and comes back with the GROWN capacity, not the original 9");
	}
}


/* =================== a complementary pair of engines ==================== */

/*
 * The wire-format claim of §3, exercised rather than asserted: two engines
 * with MIRRORED limits agree on the header width and can talk to each other,
 * while keeping their own directional limits.
 *
 *     Peer A: RX 1024, TX 64    -> length_size 2
 *     Peer B: RX 64,   TX 1024  -> length_size 2
 *
 * Nothing here fakes a link: what A's transport was handed is fed straight
 * into B's consume().
 */
void testComplementaryPeers()
{
	g_strategy = "pair";
	using A = cobs::Endpoint<cobs::Heap<cobs::Format<1024, 64>>>;
	using B = cobs::Endpoint<cobs::Heap<cobs::Format<64, 1024>>>;

	static_assert(A::length_size == 2 && B::length_size == 2,
	              "the larger limit picks the width, so the pair agrees");
	static_assert(A::max_receive_size == 1024 && A::max_send_size == 64,
	              "while A keeps its own directional limits");
	static_assert(B::max_receive_size == 64 && B::max_send_size == 1024,
	              "and B keeps the mirrored ones");
	check(true, "mirrored limits agree on the wire header and disagree on capacity");

	A a;
	B b;
	FakeTransport ta;
	FakeTransport tb;
	check(a.bind(sender_for<A>(ta), busy_for<A>(ta)), "A binds");
	check(b.bind(sender_for<B>(tb), busy_for<B>(tb)), "B binds");

	{	// A -> B, at A's maximum send size.
		const auto out = pattern(0x10, A::max_send_size);
		auto msg = a.make_message();
		check(msg.append_bytes(std::span<const uint8_t>{out}), "A builds a 64-byte frame");
		check(a.send(msg) == cobs::SendResult::Sent, "and sends it");

		b.consume(std::span<const uint8_t>{ta.sent.back()});
		const auto got = b.pop_packet();
		check(got.size() == out.size(), "B receives it whole");
		check(std::vector<uint8_t>(got.data().begin(), got.data().end()) == out,
		      "byte for byte");
		ta.finish();
		a.poll();
	}
	{	// B -> A, at a size only this direction allows: 300 bytes is legal for
		// B to send and for A to receive, and illegal in the other direction.
		const auto out = pattern(0x40, 300);
		auto msg = b.make_message();
		check(msg.append_bytes(std::span<const uint8_t>{out}), "B builds a 300-byte frame");
		check(b.send(msg) == cobs::SendResult::Sent, "and sends it");

		a.consume(std::span<const uint8_t>{tb.sent.back()});
		const auto got = a.pop_packet();
		check(got.size() == 300, "A receives all 300 bytes");
		check(std::vector<uint8_t>(got.data().begin(), got.data().end()) == out,
		      "byte for byte, in the direction that allows it");

		auto too_big = a.make_message();
		const auto over = pattern(0x50, 65);
		check(too_big.append_bytes(std::span<const uint8_t>{over}) == false,
		      "while A still refuses to SEND more than 64");
		tb.finish();
		b.poll();
	}
	{	// And the limit that is not shared: 300 bytes arriving at B, whose
		// rx_max_size is 64, is refused before any allocation.
		const auto out = pattern(0x60, 300);
		auto msg = b.make_message();
		check(msg.append_bytes(std::span<const uint8_t>{out}), "B builds another 300 bytes");
		check(b.send(msg) == cobs::SendResult::Sent, "and sends it");

		const std::size_t before = b.stats().rx.oversize;
		b.consume(std::span<const uint8_t>{tb.sent.back()}); // fed to itself
		check(b.stats().rx.oversize == before + 1,
		      "B's own RX refuses a frame its TX side was free to build");
		check(!b.has_packet(), "and nothing is delivered");
		tb.finish();
		b.poll();
	}
}

void testStorageDoesNotChangeFormat()
{
	g_strategy = "format";
	using Wire = cobs::Format<64>;
	using HeapEngine = cobs::Endpoint<cobs::Heap<Wire>>;
	using PoolEngine = cobs::Endpoint<cobs::Pool<2, 1, Wire>>;

	static_assert(std::is_same_v<typename HeapEngine::Format, Wire>);
	static_assert(std::is_same_v<typename PoolEngine::Format, Wire>);
	static_assert(HeapEngine::length_size == PoolEngine::length_size);

	HeapEngine heap;
	PoolEngine pool;
	FakeTransport heap_transport;
	FakeTransport pool_transport;
	bind(heap, heap_transport);
	bind(pool, pool_transport);

	const std::vector<uint8_t> payload{0x11, 0x00, 0x22, 0x33, 0x00, 0x44};
	auto heap_message = heap.make_message(payload.size());
	auto pool_message = pool.make_message(payload.size());
	check(heap_message.append_bytes(std::span<const uint8_t>{payload}),
	      "heap message accepts the payload");
	check(pool_message.append_bytes(std::span<const uint8_t>{payload}),
	      "pool message accepts the same payload");
	check(heap.send(heap_message) == cobs::SendResult::Sent, "heap sends");
	check(pool.send(pool_message) == cobs::SendResult::Sent, "pool sends");
	check(heap_transport.sent.back() == pool_transport.sent.back(),
	      "one Format produces byte-identical frames across storage strategies");

	heap_transport.finish();
	pool_transport.finish();
	heap.poll();
	pool.poll();
}

/* =============== the chosen owning-delegate boundary =================== */

void testDelegateBindingModes()
{
	g_strategy = "delegate";
	using Engine = cobs::Endpoint<cobs::Heap<cobs::Format<32>>>;
	const std::vector<uint8_t> payload{0x11, 0x00, 0x22};

	{	// Ordinary callables are owned, including a move-only capture.
		Engine endpoint;
		std::vector<uint8_t> captured;
		int marker = 0;
		bool busy = false;
		{
			auto send_callable =
				[&captured, &marker, stamp = std::make_unique<int>(0x5A)](
					const std::span<const uint8_t> frame) noexcept {
					marker = *stamp;
					captured.assign(frame.begin(), frame.end());
					return true;
				};
			auto busy_callable = [&busy]() noexcept { return busy; };
			typename Engine::Sender sender{std::move(send_callable)};
			typename Engine::BusyQuery query{std::move(busy_callable)};
			check(!sender.non_owning() && !query.non_owning(),
			      "capturing callables select owning delegate storage");
			check(endpoint.bind(std::move(sender), std::move(query)),
			      "an owning delegate pair binds");
		}

		auto message = endpoint.make_message(payload.size());
		check(message.append_bytes(std::span<const uint8_t>{payload}),
		      "a message is built after the source callables left scope");
		check(endpoint.send(message) == cobs::SendResult::Sent && marker == 0x5A &&
		          captured == cobs_test::frame(payload, Engine::length_size),
		      "the endpoint still owns and invokes the move-only lambda");
		endpoint.poll();
		check(!endpoint.tx_active(), "the owned busy query remains callable too");
	}

	{	// Member binding intentionally borrows the long-lived object.
		Engine endpoint;
		FakeTransport transport;
		auto sender = tiny::bind<&FakeTransport::send>(transport);
		auto query = tiny::bind<&FakeTransport::tx_busy>(transport);
		static_assert(std::is_same_v<decltype(sender), typename Engine::Sender>);
		static_assert(std::is_same_v<decltype(query), typename Engine::BusyQuery>);
		check(sender.non_owning() && query.non_owning(),
		      "member binding keeps explicit non-owning semantics");
		check(endpoint.bind(std::move(sender), std::move(query)),
		      "a bound-member pair binds without an adapter");
		auto message = endpoint.make_message(payload.size());
		check(message.append_bytes(std::span<const uint8_t>{payload}),
		      "the bound-member message is built");
		check(endpoint.send(message) == cobs::SendResult::Sent &&
		          transport.sent.back() == cobs_test::frame(payload, Engine::length_size),
		      "the endpoint invokes the bound transport methods");
		transport.finish();
		endpoint.poll();
	}

	{	// borrow() keeps observing the original callable objects, not copies.
		Engine endpoint;
		FakeTransport transport;
		auto send_callable = [&transport](const std::span<const uint8_t> frame) noexcept {
			return transport.send(frame);
		};
		auto busy_callable = [&transport]() noexcept { return transport.tx_busy(); };
		typename Engine::Sender sender{tiny::borrow(send_callable)};
		typename Engine::BusyQuery query{tiny::borrow(busy_callable)};
		check(sender.non_owning() && query.non_owning(),
		      "borrowed callables remain explicitly non-owning");
		check(endpoint.bind(std::move(sender), std::move(query)),
		      "a borrowed callable pair binds");
		auto message = endpoint.make_message(payload.size());
		check(message.append_bytes(std::span<const uint8_t>{payload}),
		      "the borrowed-callable message is built");
		transport.busy = true;
		check(endpoint.send(message) == cobs::SendResult::Busy,
		      "the borrowed busy callable observes later external state");
		transport.busy = false;
		check(endpoint.send(message) == cobs::SendResult::Sent,
		      "the same borrowed pair starts the transfer once external state changes");
		transport.finish();
		endpoint.poll();
	}
}

} // namespace

int main()
{
	group("Engine");
	runEngine<cobs::Heap<cobs::Format<64>>>("heap");
	runEngine<cobs::Pool<4, 2, cobs::Format<64>>>("fixed");
	group("DelegateLifetime");
	testDelegateBindingModes();

	group("FixedSpecific");
	testFixedExhaustion();
	testDefaultCapacityHint();
	testComplementaryPeers();
	testStorageDoesNotChangeFormat();
	testReportedCapacitySurvivesTheEngine();
	testDestructorReclaimsActiveTx();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
