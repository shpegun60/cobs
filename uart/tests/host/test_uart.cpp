/*
 * Executable verification for uart/Uart.h, organised by the GUARANTEE under
 * test rather than by HAL function, so the suite survives refactoring inside
 * the driver.
 */
#define UART_ENGINE_IMPLEMENT
#include "uart_test_fixture.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if !UART_ENGINE_INTERNAL_CALLBACKS_ON && !(USE_HAL_UART_REGISTER_CALLBACKS == 1)
extern "C" void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* const huart,
		const uint16_t size)
{
	uart::detail::Registry::onRxEvent(huart, size);
}

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* const huart)
{
	uart::detail::Registry::onTxCplt(huart);
}

extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* const huart)
{
	uart::detail::Registry::onError(huart);
}
#endif

namespace {

int g_failures = 0;
int g_checks = 0;
const char* g_group = "";

void group(const char* name) { g_group = name; std::printf("\n[%s]\n", name); }

void check(const bool ok, const std::string& what)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("  FAIL  %s\n", what.c_str());
	} else {
		std::printf("  ok    %s\n", what.c_str());
	}
}

// Every test ends by asserting the fake HAL saw no ownership violation.
void checkNoViolations(const std::string& what)
{
	const auto& v = fake::model().violations;
	check(v.empty(), what + (v.empty() ? "" : (" -> " + v.front())));
}

/* ============================ Initialization ============================ */

void testRegistryRejectsInvalidEntriesAndNullCallbacks()
{
	struct Probe { uint32_t rx = 0; uint32_t tx = 0; uint32_t error = 0; };
	const uart::detail::Registry::Ops ops = {
		[](void* p, uint16_t n) noexcept { static_cast<Probe*>(p)->rx += n; },
		[](void* p) noexcept { ++static_cast<Probe*>(p)->tx; },
		[](void* p) noexcept { ++static_cast<Probe*>(p)->error; },
	};

	// This used to match the first zero-initialized slot and call a null thunk.
	uart::detail::Registry::onRxEvent(nullptr, 1u);
	uart::detail::Registry::onTxCplt(nullptr);
	uart::detail::Registry::onError(nullptr);
	check(true, "null HAL callbacks are harmless no-ops");

	UART_HandleTypeDef no_peripheral{};
	Probe p{};
	check(!uart::detail::Registry::attach(&no_peripheral, &p, ops),
	      "the registry refuses a handle without a peripheral instance");
	check(!uart::detail::Registry::attach(nullptr, &p, ops),
	      "the registry refuses a null handle");
	check(!uart::detail::Registry::attach(&no_peripheral, nullptr, ops),
	      "the registry refuses a null target");

	USART_TypeDef instance{};
	UART_HandleTypeDef valid{};
	valid.Instance = &instance;
	check(!uart::detail::Registry::attach(&valid, &p, {}),
	      "the registry refuses null operation thunks");
	check(uart::detail::Registry::attach(&valid, &p, ops),
	      "a complete registry entry attaches");
	uart::detail::Registry::onRxEvent(&valid, 7u);
	uart::detail::Registry::onTxCplt(&valid);
	uart::detail::Registry::onError(&valid);
	check(p.rx == 7u && p.tx == 1u && p.error == 1u,
	      "all registry operations dispatch to the right target");

	UART_HandleTypeDef alias{};
	alias.Instance = &instance;
	Probe alias_probe{};
	check(!uart::detail::Registry::attach(&alias, &alias_probe, ops),
	      "a second handle cannot claim the same physical UART");
	USART_TypeDef second_instance{};
	UART_HandleTypeDef second_handle{};
	second_handle.Instance = &second_instance;
	check(!uart::detail::Registry::attach(&second_handle, &p, ops),
	      "one target cannot claim two different UART entries");
	uart::detail::Registry::detach(&p);
	uart::detail::Registry::detach(nullptr);

	std::array<USART_TypeDef, UART_ENGINE_MAX_INSTANCES + 1u> instances{};
	std::array<UART_HandleTypeDef, UART_ENGINE_MAX_INSTANCES + 1u> handles{};
	std::array<Probe, UART_ENGINE_MAX_INSTANCES + 1u> probes{};
	bool filled = true;
	for (std::size_t i = 0; i < UART_ENGINE_MAX_INSTANCES; ++i) {
		handles[i].Instance = &instances[i];
		filled = uart::detail::Registry::attach(&handles[i], &probes[i], ops) && filled;
	}
	handles[UART_ENGINE_MAX_INSTANCES].Instance = &instances[UART_ENGINE_MAX_INSTANCES];
	check(filled, "every configured registry slot can be filled");
	check(!uart::detail::Registry::attach(&handles[UART_ENGINE_MAX_INSTANCES],
	                                     &probes[UART_ENGINE_MAX_INSTANCES], ops),
	      "registry capacity exhaustion fails without overwriting an entry");
	for (std::size_t i = 0; i < UART_ENGINE_MAX_INSTANCES; ++i) {
		uart::detail::Registry::detach(&probes[i]);
	}
}

void testInitAcceptsValidConfig()
{
	fake::reset();
	Fixture f;
	check(f.start(), "a CubeMX-shaped configuration is accepted");
	check(fake::model().rx_armed, "reception is armed once init returns");
	check((f.ch_rx.dummy & DMA_IT_HT) == 0u,
	      "RX half-transfer IRQ is disabled after the first HAL arm");
#if USE_HAL_UART_REGISTER_CALLBACKS == 1
	check(f.huart.RxEventCallback != nullptr && f.huart.TxCpltCallback != nullptr &&
	      f.huart.ErrorCallback != nullptr,
	      "all per-handle HAL callbacks are registered");
#endif
	checkNoViolations("no violations during init");
}

void testInitAcceptsNineBitsWithParity()
{
	fake::reset();
	Fixture f;
	f.configure();
	f.huart.Init.WordLength = UART_WORDLENGTH_9B;
	f.huart.Init.Parity = UART_PARITY_EVEN;
	check(f.uart.init(&f.huart),
	      "9-bit framing with parity still transports all eight payload bits");
}

#if USE_HAL_UART_REGISTER_CALLBACKS == 1
void testRegisteredCallbackFailureIsTransactional()
{
	for (int fail_at = 1; fail_at <= 3; ++fail_at) {
		fake::reset();
		Fixture f;
		f.configure();
		fake::model().fail_callback_registration = fail_at;
		check(!f.uart.init(&f.huart),
		      "each HAL callback registration position can fail init");
		check(f.uart.instance() == nullptr,
		      "partial callback registration leaves the engine unbound");
		fake::model().fail_callback_registration = 0;
		check(f.uart.init(&f.huart),
		      "the released registry slot permits a clean retry");
		check(fake::model().rx_armed,
		      "the retry arms reception through registered callbacks");
	}
}
#endif

// Every rejection init() is supposed to make, as a table.
void testInitRefusalMatrix()
{
	struct Case { const char* name; void (*bend)(Fixture&); };
	const Case cases[] = {
		{"RX DMA has no instance", [](Fixture& f) { f.dma_rx.Instance = nullptr; }},
		{"TX DMA has no instance", [](Fixture& f) { f.dma_tx.Instance = nullptr; }},
		{"circular RX DMA",        [](Fixture& f) { f.dma_rx.Init.Mode = DMA_CIRCULAR; }},
		{"circular TX DMA",        [](Fixture& f) { f.dma_tx.Init.Mode = DMA_CIRCULAR; }},
		{"RX and TX share a channel", [](Fixture& f) { f.huart.hdmatx = &f.dma_rx; }},
		{"RX and TX alias one hardware channel", [](Fixture& f) { f.dma_tx.Instance = f.dma_rx.Instance; }},
		{"DMA Parent not linked",  [](Fixture& f) { f.dma_rx.Parent = nullptr; }},
		{"half-word memory width", [](Fixture& f) { f.dma_rx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD; }},
		{"half-word periph width", [](Fixture& f) { f.dma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD; }},
		{"peripheral increment on", [](Fixture& f) { f.dma_rx.Init.PeriphInc = DMA_PINC_ENABLE; }},
		{"memory increment off",   [](Fixture& f) { f.dma_tx.Init.MemInc = DMA_MINC_DISABLE; }},
		{"reversed RX direction",  [](Fixture& f) { f.dma_rx.Init.Direction = DMA_MEMORY_TO_PERIPH; }},
		{"DMA handle not READY",   [](Fixture& f) { f.dma_tx.State = HAL_DMA_STATE_RESET; }},
		{"8 bits WITH parity",     [](Fixture& f) { f.huart.Init.Parity = UART_PARITY_EVEN; }},
		{"invalid parity value",   [](Fixture& f) { f.huart.Init.Parity = 0xFFFFFFFFu; }},
		{"RX-only mode",           [](Fixture& f) { f.huart.Init.Mode = UART_MODE_RX; }},
		{"invalid flow control",   [](Fixture& f) { f.huart.Init.HwFlowCtl = 0xFFFFFFFFu; }},
		{"half-duplex (HDSEL)",    [](Fixture& f) { f.usart.CR3 |= USART_CR3_HDSEL; }},
		{"peripheral already busy", [](Fixture& f) { f.huart.gState = HAL_UART_STATE_BUSY_TX; }},
		{"zero baud rate",         [](Fixture& f) { f.huart.Init.BaudRate = 0; }},
	};

	for (const auto& c : cases) {
		fake::reset();
		Fixture f;
		f.configure();
		c.bend(f);
		const bool refused = !f.uart.init(&f.huart);
		check(refused, std::string("refused: ") + c.name);
		check(f.uart.instance() == nullptr,
		      std::string("stays unbound after refusing: ") + c.name);
	}
}

void testDuplicateAndRebind()
{
	fake::reset();
	Fixture f;
	check(f.start(), "first engine binds the handle");

	TestUart second;
	const bool refused = !second.init(&f.huart);
	check(refused, "a second engine is refused the same handle");
	check(second.instance() == nullptr,
	      "the refused engine stays unbound, so its destructor cannot abort a foreign UART");

	const bool rebind = !f.uart.init(&f.huart);
	check(rebind, "re-binding an already bound engine is refused");
}

/* ============================= RX ownership ============================= */

void testRxIdleAndTc()
{
	fake::reset();
	Fixture f;
	f.start();

	fake::rx_bytes("abc", 3);
	fake::rx_idle();
	f.loop();
	check(rxText() == "abc", "an IDLE-terminated chunk reaches the handler intact");

	std::string full(kChunk, 'x');
	fake::rx_bytes(full.data(), full.size());
	fake::rx_tc();
	f.loop();
	check(rxText() == "abc" + full, "a TC-terminated full chunk is delivered whole");
	check((f.ch_rx.dummy & DMA_IT_HT) == 0u,
	      "RX half-transfer IRQ is disabled again after re-arm");
	checkNoViolations("no ownership violation across IDLE and TC");
}

void testNormalRxSkipsEventTypeLookup()
{
	fake::reset();
	Fixture f;
	f.start();

	fake::rx_bytes("a", 1);
	fake::rx_idle();
	check(fake::model().rx_event_type_calls == 0u,
	      "normal IDLE takes no HAL GetRxEventType call");
	f.loop();
	fake::rx_half();
#if UART_ENGINE_HAS_RXEVENT_TYPE
	check(fake::model().rx_event_type_calls == 1u,
	      "a stray live HT pays the event-type lookup only on the cold branch");
#else
	check(fake::model().rx_event_type_calls == 0u,
	      "the old-HAL fallback derives HT entirely from RxState");
#endif
	check(fake::model().rx_armed, "HT leaves the current receive transfer armed");
}

void testCorruptDmaCountCannotEscapeTheChunk()
{
	fake::reset();
	Fixture f;
	f.start();

	fake::rx_bytes("bad", 3);
	fake::rx_corrupt_counter(static_cast<uint32_t>(kChunk + 1u));
	f.loop();

	check(rxText().empty(), "an impossible DMA count publishes no bytes");
	check(events() == "gap", "an impossible DMA count becomes one ordered gap");
	check(f.uart.stats().rx_errors == 1u, "the counter corruption is diagnosed");
	check(fake::model().rx_armed, "the receiver restarts after counter corruption");
	checkNoViolations("counter corruption never violates buffer ownership");
}

void testProceedIsSafeAgainstHandlerReentry()
{
	fake::reset();
	Fixture f;
	f.start();

	std::string seen;
	uint32_t calls = 0u;
	f.uart.setRxHandler([&](std::span<const uint8_t> bytes) noexcept {
		fake::note_consumer_sees(bytes.data());
		++calls;
		seen.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		if (calls == 1u) {
			fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1u);
			f.uart.proceed(fake::model().tick);
		}
		fake::note_consumer_done(bytes.data());
	});

	fake::rx_bytes("A", 1); fake::rx_idle();
	fake::rx_bytes("B", 1); fake::rx_idle();
	f.loop();
	fake::rx_bytes("C", 1); fake::rx_idle();
	f.loop();

	check(calls == 3u && seen == "ABC",
	      "recursive proceed neither repeats nor drops a queued chunk");
	checkNoViolations("handler reentry preserves DMA/consumer ownership");
}

void testDmaBufferNeverVisibleToConsumer()
{
	fake::reset();
	Fixture f;
	f.start();
	for (int i = 0; i < 20; ++i) {
		fake::rx_bytes("hello", 5);
		fake::rx_idle();
		f.loop();
	}
	check(fake::model().rx_data.size() == 20, "every chunk was delivered");
	checkNoViolations("DMA-owned memory was never handed to the consumer");
}

// The pool must not shrink: a leaked claim would reduce the number of distinct
// buffers the driver cycles through.
void testSlotConservation()
{
	fake::reset();
	Fixture f;
	f.start();
	for (int i = 0; i < 50; ++i) {
		fake::rx_bytes("z", 1);
		fake::rx_idle();
		f.loop();
	}
	const std::size_t distinct = fake::distinct_chunks_armed();
	check(distinct == kChunks,
	      "the driver cycles through exactly ChunkCount buffers (saw " +
	          std::to_string(distinct) + ")");
}

/* =========================== RX discontinuity =========================== */

void testOverflowProducesOrderedGap()
{
	fake::reset();
	Fixture f;
	f.start();

	// Fill every slot without draining: the consumer never runs.
	for (std::size_t i = 0; i < kChunks; ++i) {
		fake::rx_bytes("A", 1);
		fake::rx_idle();
	}
	// The pool is dry now; these bytes are physically lost.
	fake::rx_bytes("LOST", 4);
	fake::rx_idle();

	f.loop();   // drains the queued chunks, then announces the gap
	fake::rx_bytes("B", 1);
	fake::rx_idle();
	f.loop();

	const std::string seq = events();
	const std::string expected = "data:1|data:1|data:1|data:1|gap|data:1";
	check(seq == expected, "pre-gap data, then the gap, then post-gap data (got " + seq + ")");
	check(f.uart.stats().rx_overrun > 0, "the overflow is counted");
}

void testRxErrorProducesGapBeforeNextData()
{
	fake::reset();
	Fixture f;
	f.start();

	fake::rx_bytes("ok", 2);
	fake::rx_idle();
	f.loop();

	fake::rx_bytes("part", 4);   // partly filled chunk...
	fake::rx_error(HAL_UART_ERROR_ORE); // ...thrown away by a blocking error
	f.loop();

	fake::rx_bytes("next", 4);
	fake::rx_idle();
	f.loop();

	const std::string seq = events();
	check(seq == "data:2|gap|data:4", "an aborted partial chunk becomes a gap (got " + seq + ")");
	check(rxText() == "oknext", "the discarded bytes are not delivered");
}

/* ============================= TX ownership ============================= */

void testTxSingleTerminalEvent()
{
	fake::reset();
	Fixture f;
	f.start();

	const uint8_t frame[4] = {1, 2, 3, 0};
	check(f.uart.send(std::span<const uint8_t>{frame, 4}), "send starts a transfer");
	check((f.ch_tx.dummy & DMA_IT_HT) == 0u,
	      "TX half-transfer IRQ is disabled after the HAL start");
	check(f.uart.tx_busy(), "tx_busy is true while the DMA reads the caller's memory");

	fake::tx_done();
	check(!f.uart.tx_busy(), "ownership returns on completion");
	check(fake::model().tx_results.size() == 1 && fake::model().tx_results[0],
	      "exactly one terminal event, reporting success");
}

void testTxRefusesOversizedFrame()
{
	fake::reset();
	Fixture f;
	f.start();
	static std::vector<uint8_t> huge(70000, 0x41);
	const bool refused = !f.uart.send(std::span<const uint8_t>{huge.data(), huge.size()});
	check(refused, "a frame that cannot be expressed in the HAL u16 length is refused");
	check(!f.uart.tx_busy(), "no transfer was started");
}

/* ========================= Teardown arbitration ========================= */

// An RX teardown must not consume a genuine TX completion.
void testRxTeardownDoesNotEatTxCompletion()
{
	fake::reset();
	Fixture f;
	f.start();

	const uint8_t frame[2] = {7, 0};
	f.uart.send(std::span<const uint8_t>{frame, 2});

	// Force the driver into an RX restart, and have the TX complete while it
	// is tearing the receiver down.
	fake::model().rx_cplt_inside_abort = true;
	fake::rx_error(HAL_UART_ERROR_ORE);
	fake::tx_done();
	f.loop();

	check(fake::model().tx_results.size() == 1 && fake::model().tx_results[0],
	      "the TX completion survives an RX teardown and reports success once");
	check(!f.uart.tx_busy(), "TX ownership was returned");
}

// A completion raised BY the abort must not be mistaken for a real one.
void testCompletionRaisedInsideAbortIsIgnored()
{
	fake::reset();
	Fixture f;
	f.start();

	fake::rx_bytes("data", 4);
	fake::model().rx_cplt_inside_abort = true; // abort will raise the RX callback
	fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1);
	f.loop();
	f.loop();

	check(fake::model().rx_armed, "reception is running again after the teardown");
	checkNoViolations("no ownership violation from a callback raised inside an abort");
}

/* ============================ Fault injection =========================== */

void testFailedAbortKeepsOwnership()
{
	fake::reset();
	Fixture f;
	f.start();

	fake::rx_bytes("xy", 2);
	fake::model().fail_abort_receive = 1; // the abort will report HAL_TIMEOUT
    fake::rx_error(HAL_UART_ERROR_DMA);
	f.loop();

	checkNoViolations("a failed abort never releases a chunk");
	check(fake::model().dma_init_calls == 1u,
	      "an abort timeout repairs the stopped DMA handle explicitly");
	// The driver must keep retrying until the hardware really stops.
	f.loop();
	check(fake::model().rx_armed, "reception recovers once the abort succeeds");
}

void testPersistentRxRepairFailureKeepsOwnership()
{
	fake::reset();
	Fixture f;
	f.start();
	fake::rx_bytes("live", 4);
	fake::model().fail_abort_receive = 100;
	fake::model().fail_dma_init = 100;

	check(!f.uart.setBaudRate(1000000u),
	      "a baud change fails while RX hardware cannot be stopped or repaired");
	check(fake::model().rx_armed,
	      "the fake DMA still owns the partial RX buffer after failed repair");
	f.loop();
	check(rxText().empty() && events().empty(),
	      "no live RX buffer or premature gap is exposed while repair fails");
	checkNoViolations("persistent RX repair failure preserves DMA ownership");

	fake::model().fail_abort_receive = 0;
	fake::model().fail_dma_init = 0;
	f.loop();
	f.loop();
	check(fake::model().rx_armed, "RX restarts after the hardware becomes repairable");
	check(events() == "gap" && rxText().empty(),
	      "the discarded live prefix becomes exactly one gap after safe stop");
}

void testFailedInitialArmDoesNotInventGapOnRetry()
{
	fake::reset();
	Fixture f;
	f.configure();
	f.uart.setRxGapHandler([]() noexcept { fake::model().rx_events.push_back("gap"); });
	fake::model().fail_arm = 1;

	check(!f.uart.init(&f.huart), "a refused first arm leaves init failed");
	check(f.uart.init(&f.huart), "the same object retries successfully");
	f.loop();
	check(fake::model().rx_events.empty(),
	      "a transfer that never started does not manufacture a stream gap");
}

void testFailedArmDoesNotLeakSlot()
{
	fake::reset();
	Fixture f;
	f.start();

	fake::model().fail_arm = 2; // two re-arm attempts fail
	fake::rx_bytes("q", 1);
	fake::rx_idle();
	f.loop();
	f.loop();
	f.loop();

	check(fake::model().rx_armed, "the driver recovers from failed re-arms");
	for (int i = 0; i < 30; ++i) {
		fake::rx_bytes("w", 1);
		fake::rx_idle();
		f.loop();
	}
	const std::size_t distinct = fake::distinct_chunks_armed();
	check(distinct == kChunks,
	      "the pool still holds every slot after failed arms (saw " +
	          std::to_string(distinct) + ")");
}

/* ========================== TX liveness watchdog ======================== */

// Runs `n` audit periods.
static void audits(Fixture& f, int n)
{
	for (int i = 0; i < n; ++i) {
		fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1);
		f.loop();
	}
}

// A long but healthy transfer: the counter keeps moving, so it must never trip
// however long it takes. This is the property a frame-length deadline could
// not express.
void testProgressingTransferNeverTrips()
{
	fake::reset();
	Fixture f;
	f.start();

	static std::vector<uint8_t> big(4096, 0x5A);
	f.uart.send(std::span<const uint8_t>{big.data(), big.size()});
	for (int i = 0; i < 20; ++i) {
		fake::tx_progress(64);
		audits(f, 1);
	}
	check(f.uart.tx_busy(), "a slowly progressing transfer is left alone");
	check(fake::model().tx_results.empty(), "no terminal event was invented");
}

void testFrozenCounterTrips()
{
	fake::reset();
	Fixture f;
	f.start();

	static std::vector<uint8_t> big(4096, 0x5A);
	f.uart.send(std::span<const uint8_t>{big.data(), big.size()});
	fake::tx_progress(100); // moves once, then wedges
	// A single debounce now: the stall counter IS the debounce, it is not fed
	// into m_failCounter as well.
	audits(f, UART_ENGINE_FAIL_THRESHOLD + 2);

	check(!f.uart.tx_busy(), "a stalled transfer is reclaimed after one debounce");
	check(fake::model().tx_results.size() == 1 && !fake::model().tx_results[0],
	      "exactly one terminal event, reporting failure");
	check(fake::model().rx_armed, "the receiver keeps running — a TX stall is TX-only");
}

// The DMA drained but the UART is still shifting: NOT a completion yet.
void testDmaDrainedWithoutTcHasBoundedTail()
{
	fake::reset();
	Fixture f;
	f.start();

	const uint8_t frame[4] = {1, 2, 3, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});
	fake::tx_dma_done(); // counter hits 0, TC still clear
	audits(f, 2);

	check(f.uart.tx_busy(), "an empty counter alone is not treated as completion");
	check(fake::model().tx_results.empty(), "no terminal event while the line still shifts");

	// TC can also fail permanently (stopped peripheral clock, wedged UART).
	// Once the conservative FIFO/shift-register budget expires, ownership must
	// not remain borrowed forever.
	audits(f, UART_ENGINE_FAIL_THRESHOLD);
	check(!f.uart.tx_busy(), "a post-DMA UART tail without TC is eventually reclaimed");
	check(fake::model().tx_results.size() == 1u && !fake::model().tx_results[0],
	      "a wedged post-DMA tail reports one failure");
	check(fake::model().rx_armed, "post-DMA TX recovery leaves RX running");
}

void testLowBaudDrainBudgetIsConservative()
{
	fake::reset();
	Fixture f;
	f.configure();
	f.huart.Init.BaudRate = 300u;
	check(f.uart.init(&f.huart), "300-baud drain fixture starts");
	f.uart.setTxHandler([](bool ok) { fake::model().tx_results.push_back(ok); });

	const uint8_t frame[4] = {1, 2, 3, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});
	fake::tx_dma_done();
	audits(f, 2 * UART_ENGINE_FAIL_THRESHOLD);
	check(f.uart.tx_busy(), "a slow hardware FIFO tail is not timed out early");
	f.huart.Instance->ISR |= USART_ISR_TC;
	audits(f, 1);
	check(fake::model().tx_results.size() == 1u && fake::model().tx_results[0],
	      "the eventual low-baud TC is reported as success");
}

// remaining == 0 AND TC set means the last stop bit is already on the wire:
// the frame WAS delivered and only the notification went missing, so the
// synthetic completion must report SUCCESS.
void testLostCompletionIsSyntheticSuccess()
{
	fake::reset();
	Fixture f;
	f.start();

	const uint8_t frame[4] = {1, 2, 3, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});
	fake::tx_dma_done();
	f.huart.Instance->ISR |= USART_ISR_TC; // the line finished...
	// ...but the completion interrupt never arrives.
	audits(f, UART_ENGINE_FAIL_THRESHOLD + 1);

	check(!f.uart.tx_busy(), "a lost completion is detected and ownership returned");
	check(fake::model().tx_results.size() == 1 && fake::model().tx_results[0],
	      "the caller is told the frame SUCCEEDED — it was physically sent");
	check(fake::model().rx_armed, "the receiver was not torn down for a TX event");
}

void testCtsFrozenCounterNeverTrips()
{
	fake::reset();
	Fixture f;
	f.configure();
	f.huart.Init.HwFlowCtl = UART_HWCONTROL_RTS_CTS;
	check(f.uart.init(&f.huart), "CTS flow control is a valid configuration");
	f.uart.setTxHandler([](bool ok) { fake::model().tx_results.push_back(ok); });

	const uint8_t frame[4] = {1, 2, 3, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});
	audits(f, 4 * UART_ENGINE_FAIL_THRESHOLD); // the peer holds the line

	check(f.uart.tx_busy(), "a peer holding CTS never looks like a stall");
	check(fake::model().tx_results.empty(), "no terminal event was invented under CTS");
	fake::tx_dma_done();
	audits(f, 4 * UART_ENGINE_FAIL_THRESHOLD);
	check(f.uart.tx_busy(), "CTS may also hold a fully DMA-fed UART tail indefinitely");
	check(fake::model().tx_results.empty(), "post-DMA CTS hold invents no failure");
}

// Stall detection is off under CTS, but a physically finished frame whose
// completion got lost is still a fault.
void testCtsStillDetectsLostCompletion()
{
	fake::reset();
	Fixture f;
	f.configure();
	f.huart.Init.HwFlowCtl = UART_HWCONTROL_RTS_CTS;
	f.uart.init(&f.huart);
	f.uart.setTxHandler([](bool ok) { fake::model().tx_results.push_back(ok); });

	const uint8_t frame[4] = {1, 2, 3, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});
	fake::tx_dma_done();
	f.huart.Instance->ISR |= USART_ISR_TC;
	audits(f, UART_ENGINE_FAIL_THRESHOLD + 1);

	check(!f.uart.tx_busy(), "under CTS a lost completion is still detected");
	check(fake::model().tx_results.size() == 1 && fake::model().tx_results[0],
	      "and is still reported as success");
}

#include "test_uart_races.inc"
#include "test_uart_baud.inc"
#include "test_uart_torture.inc"

/* ============================== Watchdog =============================== */

void testWatchdogRevivesDeadReceiver()
{
	fake::reset();
	Fixture f;
	f.start();

	// The receiver dies silently: hardware stopped, nobody told the driver.
	fake::model().rx_armed = false;
	f.huart.RxState = HAL_UART_STATE_READY;

	for (int i = 0; i < UART_ENGINE_FAIL_THRESHOLD + 1; ++i) {
		fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1);
		f.loop();
	}
	check(fake::model().rx_armed, "the watchdog restarts a silently dead receiver");
	check(f.uart.stats().restarts > 1, "the restart is counted");
}

void testWatchdogRejectsEveryNonBusyRxState()
{
	fake::reset();
	Fixture f;
	f.start();

	// RESET is neither the ordinary READY failure nor a valid armed state.
	f.huart.RxState = HAL_UART_STATE_RESET;
	for (int i = 0; i < UART_ENGINE_FAIL_THRESHOLD + 1; ++i) {
		fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1u);
		f.loop();
	}
	check(fake::model().rx_armed && f.huart.RxState == HAL_UART_STATE_BUSY_RX,
	      "the watchdog restores RESET/invalid RX state to BUSY_RX");
}

} // namespace

// `test_uart --seed 0xDEADBEEF --steps 1000000` beats the driver with a
// chair for as long as you like; the default suite runs many shorter seeds.
int main(int argc, char** argv)
{
	uint32_t one_seed = 0;
	std::size_t steps = 20000;
	for (int i = 1; i + 1 < argc; i += 2) {
		const std::string k = argv[i];
		if (k == "--seed")  { one_seed = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 0)); }
		if (k == "--steps") { steps = static_cast<std::size_t>(std::strtoul(argv[i + 1], nullptr, 0)); }
	}
	if (one_seed != 0u) {
		std::printf("\n[Torture] seed=0x%08X steps=%zu\n", one_seed, steps);
		const bool ok = torture::run(one_seed, steps);
		std::printf("%s\n", ok ? "  ok    survived" : "  FAIL");
		return ok ? 0 : 1;
	}

	group("Initialization");
	testRegistryRejectsInvalidEntriesAndNullCallbacks();
	testInitAcceptsValidConfig();
	testInitAcceptsNineBitsWithParity();
#if USE_HAL_UART_REGISTER_CALLBACKS == 1
	testRegisteredCallbackFailureIsTransactional();
#endif
	testInitRefusalMatrix();
	testDuplicateAndRebind();

	group("RxOwnership");
	testRxIdleAndTc();
	testNormalRxSkipsEventTypeLookup();
	testCorruptDmaCountCannotEscapeTheChunk();
	testProceedIsSafeAgainstHandlerReentry();
	testDmaBufferNeverVisibleToConsumer();
	testSlotConservation();

	group("RxDiscontinuity");
	testOverflowProducesOrderedGap();
	testRxErrorProducesGapBeforeNextData();

	group("TxOwnership");
	testTxSingleTerminalEvent();
	testTxRefusesOversizedFrame();

	group("TeardownArbitration");
	testRxTeardownDoesNotEatTxCompletion();
	testCompletionRaisedInsideAbortIsIgnored();

	group("FaultInjection");
	testFailedAbortKeepsOwnership();
	testPersistentRxRepairFailureKeepsOwnership();
	testFailedInitialArmDoesNotInventGapOnRetry();
	testFailedArmDoesNotLeakSlot();

	group("TxLiveness");
	testProgressingTransferNeverTrips();
	testFrozenCounterTrips();
	testDmaDrainedWithoutTcHasBoundedTail();
	testLowBaudDrainBudgetIsConservative();
	testLostCompletionIsSyntheticSuccess();
	testCtsFrozenCounterNeverTrips();
	testCtsStillDetectsLostCompletion();

	group("CrossDirection");
	testRxErrorDuringTxTeardown();
	testTxErrorDuringRxTeardown();

	group("FailureRecovery");
	testInitFailsWhenFirstArmFails();
	testSendFailureLeavesNoTrace();
	testDrainingUartWithinBudgetIsNotAStall();
	testSyntheticAndRealCompletionRaceOnce();
	testAbortTimeoutDuringStallKeepsOwnership();
	testFullRecoveryRepairsBothDmaDirections();

	group("Watchdog");
	testWatchdogRevivesDeadReceiver();
	testWatchdogRejectsEveryNonBusyRxState();

	group("BaudRate");
	testBaudRefusedWhileTransmitting();
	testBaudChangeReArmsAndGaps();
	testBaudChangePreservesFifoConfiguration();
	testFailedFifoRestoreRollsBackTheWholeChange();
	testBaudChangeRefreshesCtsWatchPolicy();
	testBaudChangeCanReenableStallDetection();
	testUnreachableBaudKeepsTheLinkAlive();
	testBaudChangeRejectsNonsense();

	group("RandomizedTorture");
	testRandomizedTorture(steps);

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
