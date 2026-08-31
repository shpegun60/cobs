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

		auto msg = cobs.get_msg();
		check(static_cast<bool>(msg), "get_msg yields a message");
		const auto payload = pattern(0x20, 6);
		const auto room = msg.reserve(payload.size());
		for (std::size_t i = 0; i < payload.size(); ++i) { room[i] = payload[i]; }

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

		auto first = cobs.get_msg();
		(void)first.reserve(1);
		check(cobs.push(first) == SendResult::Sent, "the first frame goes out");

		auto second = cobs.get_msg();
		const auto payload = pattern(0x30, 4);
		const auto room = second.reserve(payload.size());
		for (std::size_t i = 0; i < payload.size(); ++i) { room[i] = payload[i]; }

		check(cobs.push(second) == SendResult::Busy, "a second push while busy is refused");
		check(static_cast<bool>(second) && !second.encoded(),
		      "leaving the message Building, not encoded");
		check(cobs.tx_stats().send_refused_busy == 1, "and counted");

		// Still writable: the raw payload was never touched.
		const auto again = second.reserve(2);
		check(again.size() == 2, "and its payload is still writable");

		t.finish();
		cobs.proceed();
		check(cobs.push(second) == SendResult::Sent, "it sends once the link frees up");
	}

	/* --- a failed start keeps the SAME frame retryable ------------------ */
	{
		Engine cobs;
		FakeTransport t;
		bind(cobs, t);

		auto msg = cobs.get_msg();
		const auto payload = pattern(0x40, 7);
		const auto room = msg.reserve(payload.size());
		for (std::size_t i = 0; i < payload.size(); ++i) { room[i] = payload[i]; }

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
		auto msg = cobs.get_msg();
		(void)msg.reserve(2);
		check(cobs.push(msg) == SendResult::NotBound, "pushing with no transport bound is NotBound");
		check(static_cast<bool>(msg), "and the message is untouched");

		typename Engine::Msg empty;
		check(cobs.push(empty) == SendResult::Invalid, "an empty message is Invalid");

		Engine other;
		auto foreign = other.get_msg();
		(void)foreign.reserve(2);
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
			auto msg = cobs.get_msg();
			(void)msg.reserve(1);
			check(cobs.push(msg) == SendResult::NotBound,
			      "so neither half leaked into the engine");
		}

		bind(cobs, t);
		auto msg = cobs.get_msg();
		(void)msg.reserve(3);
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
		auto msg = cobs.get_msg();
		const auto room = msg.reserve(out.size());
		for (std::size_t i = 0; i < out.size(); ++i) { room[i] = out[i]; }
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

	auto a = cobs.get_msg();
	check(static_cast<bool>(a), "the single TX block is handed out");
	auto b = cobs.get_msg();
	check(!b, "a second message from a one-block pool is empty");
	check(cobs.push(b) == SendResult::Invalid, "and pushing it is Invalid, not a crash");

	(void)a.reserve(4);
	check(cobs.push(a) == SendResult::Sent, "the real one still sends");

	// The block is with the transport, so the pool is dry until it returns.
	auto c = cobs.get_msg();
	check(!c, "the pool stays dry while the transport holds the block");
	t.finish();
	cobs.proceed();
	auto d = cobs.get_msg();
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
		auto msg = cobs.get_msg();
		(void)msg.reserve(3);
		check(cobs.push(msg) == SendResult::Sent, "a frame is in flight at destruction");
		check(cobs.allocator().tx_available() == 1, "one TX block is out");
		// The transport is finished with it — precondition 2 is satisfied.
		t.finish();
	}
	check(true, "destroying the engine with a reclaimed block is clean");
}

} // namespace

int main()
{
	group("Engine");
	runEngine<CobsHeapAllocator<64, 64>>("heap");
	runEngine<CobsFixedAllocator<64, 4, 64, 2>>("fixed");

	group("FixedSpecific");
	testFixedExhaustion();
	testDestructorReclaimsActiveTx();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
