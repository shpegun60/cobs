/*
 * Randomized state-machine torture. Included by test_uart.cpp.
 *
 * The generator is STATE-AWARE: it only offers operations that make sense for
 * the current state, so iterations are not wasted completing transfers that
 * were never started. After every single operation the invariants below are
 * re-checked — the ones the harness can actually prove from the outside, not
 * ones that would need test hooks inside the driver.
 */

namespace torture {

// Deterministic PRNG: a failing run must be replayable from its seed alone.
struct Rng {
	uint32_t s;
	explicit Rng(uint32_t seed) : s(seed ? seed : 1u) {}
	uint32_t next()
	{
		s ^= s << 13; s ^= s >> 17; s ^= s << 5; // xorshift32
		return s;
	}
	uint32_t below(uint32_t n) { return next() % n; }
};

// A short trail of what led here — a bare seed turns debugging into archaeology.
struct Trail {
	static constexpr std::size_t kKeep = 48;
	std::vector<std::string> ops;
	void note(const std::string& op)
	{
		ops.push_back(op);
		if (ops.size() > kKeep) { ops.erase(ops.begin()); }
	}
};

struct State {
	Fixture f;
	Trail trail;
	std::size_t step = 0;
	uint32_t tx_generation = 0;   // one frame -> at most one terminal event
	std::size_t tx_events_seen = 0;
	std::string failure;
};

State* g_state = nullptr;

void dump(const State& st, uint32_t seed, const std::string& why)
{
	std::printf("\n  TORTURE FAILURE: %s\n", why.c_str());
	std::printf("  seed=0x%08X step=%zu\n", seed, st.step);
	const auto& m = fake::model();
	std::printf("  rx_armed=%d tx_armed=%d rx_dst=%p tx_busy=%d\n",
	            static_cast<int>(m.rx_armed), static_cast<int>(m.tx_armed),
	            static_cast<const void*>(m.rx_dst),
	            static_cast<int>(st.f.uart.tx_busy()));
	std::printf("  RxState=0x%02X gState=0x%02X ErrorCode=0x%02X rxCount=%u txCount=%u\n",
	            static_cast<unsigned>(st.f.huart.RxState),
	            static_cast<unsigned>(st.f.huart.gState),
	            static_cast<unsigned>(st.f.huart.ErrorCode),
	            static_cast<unsigned>(st.f.dma_rx.CountRemaining),
	            static_cast<unsigned>(st.f.dma_tx.CountRemaining));
	std::printf("  primask=%u tick=%u overrun=%u restarts=%u tx_events=%zu\n",
	            static_cast<unsigned>(fake::g_primask), static_cast<unsigned>(m.tick),
	            static_cast<unsigned>(st.f.uart.stats().rx_overrun),
	            static_cast<unsigned>(st.f.uart.stats().restarts),
	            st.tx_events_seen);
	if (!m.violations.empty()) {
		std::printf("  first ownership violation: %s\n", m.violations.front().c_str());
	}
	std::printf("  last %zu operations:\n   ", st.trail.ops.size());
	for (const auto& op : st.trail.ops) { std::printf(" %s", op.c_str()); }
	std::printf("\n");
}

// Only what the harness can prove from outside the driver.
bool invariantsHold(State& st)
{
	const auto& m = fake::model();
	if (!m.violations.empty()) {
		st.failure = "fake HAL reported: " + m.violations.front();
		return false;
	}
	// The consumer must never be handed the buffer the DMA is filling. (The
	// fake HAL also checks this at the moment of delivery; this catches a
	// stale marking too.)
	if (m.rx_armed && m.rx_dst != nullptr) {
		const auto it = m.slots.find(m.rx_dst);
		if (it != m.slots.end() && it->second == fake::Slot::ConsumerVisible) {
			st.failure = "the DMA is armed onto a buffer marked consumer-visible";
			return false;
		}
	}
	// Releasing TX ownership while the DMA may still be reading the frame is
	// the use-after-free class we care about most.
	if (!st.f.uart.tx_busy() && m.tx_armed) {
		st.failure = "tx_busy() is false while the fake DMA still borrows the frame";
		return false;
	}
	// One frame, at most one terminal event.
	if (st.tx_events_seen > 1) {
		st.failure = "more than one terminal event for a single TX frame";
		return false;
	}
	return true;
}

// One randomized step, chosen from what the current state actually allows.
void step(State& st, Rng& rng)
{
	const auto& m = fake::model();
	const bool tx_live = m.tx_armed;
	const bool tx_drained = tx_live && st.f.dma_tx.CountRemaining == 0u;

	enum Op { RxBytes, RxIdle, RxTc, RxErr, TxStart, TxProgress, TxDmaDone,
	          TxTc, TxErr, Tick, Proceed, MaskIrq, FailAbort, FailArm, kOpCount };

	for (int attempt = 0; attempt < 8; ++attempt) {
		const Op op = static_cast<Op>(rng.below(kOpCount));
		switch (op) {
		case RxBytes:
			if (!m.rx_armed || st.f.dma_rx.CountRemaining == 0u) { continue; }
			{
				const uint32_t n = 1u + rng.below(
					static_cast<uint32_t>(st.f.dma_rx.CountRemaining > 8u ? 8u
					                      : st.f.dma_rx.CountRemaining));
				static const char payload[] = "abcdefgh";
				fake::rx_bytes(payload, n);
				st.trail.note("rx" + std::to_string(n));
			}
			return;
		case RxIdle:
			if (!m.rx_armed) { continue; }
			fake::rx_idle(); st.trail.note("idle"); return;
		case RxTc:
			if (!m.rx_armed) { continue; }
			fake::rx_tc(); st.trail.note("tc"); return;
		case RxErr:
			fake::rx_error(rng.below(2) ? HAL_UART_ERROR_ORE : HAL_UART_ERROR_FE);
			st.trail.note("rxerr"); return;
		case TxStart: {
			if (st.f.uart.tx_busy()) { continue; }
			static const uint8_t frame[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,0};
			const std::size_t n = 1u + rng.below(16);
			if (st.f.uart.send(std::span<const uint8_t>{frame, n})) {
				++st.tx_generation;
				st.tx_events_seen = 0;
			}
			st.trail.note("tx" + std::to_string(n));
			return;
		}
		case TxProgress:
			if (!tx_live) { continue; }
			fake::tx_progress(static_cast<uint16_t>(1u + rng.below(4)));
			st.trail.note("txprog"); return;
		case TxDmaDone:
			if (!tx_live) { continue; }
			fake::tx_dma_done(); st.trail.note("txdma"); return;
		case TxTc:
			if (!tx_drained && rng.below(4) != 0u) { continue; }
			fake::tx_uart_tc(); st.trail.note("txtc"); return;
		case TxErr:
			if (!tx_live) { continue; }
			fake::tx_error(); st.trail.note("txerr"); return;
		case Tick:
			fake::advance_tick(1u + rng.below(UART_ENGINE_CHECK_PERIOD_MS + 50u));
			st.trail.note("tick"); return;
		case Proceed:
			st.f.loop(); st.trail.note("proceed"); return;
		case MaskIrq:
			// Run a whole burst with interrupts masked, then release: the
			// pending events must all be delivered on restore.
			{
				uart::detail::IrqGuard guard;
				if (m.rx_armed) { fake::rx_bytes("XY", 2); fake::rx_idle(); }
				st.trail.note("masked");
			}
			return;
		case FailAbort:
			fake::model().fail_abort_receive = static_cast<int>(rng.below(2));
			fake::model().fail_abort_transmit = static_cast<int>(rng.below(2));
			st.trail.note("failabort"); return;
		case FailArm:
			fake::model().fail_arm = static_cast<int>(rng.below(2));
			st.trail.note("failarm"); return;
		default: continue;
		}
	}
	st.f.loop(); // nothing applicable this round
	st.trail.note("proceed");
}

bool run(uint32_t seed, std::size_t steps)
{
	fake::reset();
	State st;               // holds a non-copyable Fixture: build, never assign
	g_state = &st;
	if (!st.f.start()) {
		std::printf("  TORTURE: fixture failed to start\n");
		return false;
	}
	st.f.uart.setTxHandler([](bool) {
		if (g_state) { ++g_state->tx_events_seen; }
	});

	Rng rng(seed);
	for (st.step = 0; st.step < steps; ++st.step) {
		step(st, rng);
		if (!invariantsHold(st)) {
			dump(st, seed, st.failure);
			return false;
		}
	}

	// Drain and let the driver settle, then prove the pool never shrank.
	fake::model().fail_abort_receive = 0;
	fake::model().fail_abort_transmit = 0;
	fake::model().fail_arm = 0;
	// Census restarted here: during the torture the drop buffer is armed too,
	// so counting addresses across the whole run cannot distinguish "the pool
	// shrank" from "an overflow happened". In this clean drain the consumer
	// keeps up, so only real chunks are ever armed.
	fake::clear_armed_history();
	for (int i = 0; i < 40; ++i) {
		fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1);
		st.f.loop();
		if (fake::model().rx_armed) { fake::rx_bytes("z", 1); fake::rx_idle(); }
	}
	const std::size_t distinct = fake::distinct_chunks_armed();
	if (distinct != kChunks) {
		st.failure = "the pool shrank: only " + std::to_string(distinct) +
		             " of " + std::to_string(kChunks) + " buffers still cycle";
		dump(st, seed, st.failure);
		return false;
	}
	return true;
}

} // namespace torture

// Many short runs beat one long one: different seeds explore different
// trajectories, a single 1M-step run just walks one of them further.
void testRandomizedTorture(std::size_t steps_per_seed)
{
	const uint32_t seeds[] = {
		0x00000001u, 0xDEADBEEFu, 0x12345678u, 0xC0FFEE11u,
		0xA5A5A5A5u, 0x5A5A5A5Au, 0x0BADF00Du, 0xFEEDFACEu,
		0x13579BDFu, 0x2468ACE0u, 0x7FFFFFFFu, 0x80000001u,
		0xCAFEBABEu, 0x1BADB002u, 0x31415926u, 0x27182818u,
		0x61626364u, 0x00FF00FFu, 0xFF00FF00u, 0x11111111u,
		0x99999999u, 0xABCDEF01u, 0x10FEDCBAu, 0x55555555u,
		0xAAAAAAAAu, 0x0F0F0F0Fu, 0xF0F0F0F0u, 0x3C3C3C3Cu,
		0x6D5A4C3Bu, 0x2A2A2A2Au, 0xD1CED1CEu, 0x8BADF00Du,
	};
	std::size_t ok = 0;
	for (const uint32_t seed : seeds) {
		if (torture::run(seed, steps_per_seed)) { ++ok; }
	}
	check(ok == (sizeof seeds / sizeof seeds[0]),
	      std::to_string(ok) + "/" + std::to_string(sizeof seeds / sizeof seeds[0]) +
	          " seeds survived " + std::to_string(steps_per_seed) + " random events each");
}
