/*
 * Executable verification for uart/Uart.h, organised by the GUARANTEE under
 * test rather than by HAL function, so the suite survives refactoring inside
 * the driver.
 */
#define UART_ENGINE_IMPLEMENT
#include "uart_test_fixture.h"

#include <cstdio>
#include <string>
#include <vector>

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

void testInitAcceptsValidConfig()
{
	fake::reset();
	Fixture f;
	check(f.start(), "a CubeMX-shaped configuration is accepted");
	check(fake::model().rx_armed, "reception is armed once init returns");
	checkNoViolations("no violations during init");
}

// Every rejection init() is supposed to make, as a table.
void testInitRefusalMatrix()
{
	struct Case { const char* name; void (*bend)(Fixture&); };
	const Case cases[] = {
		{"circular RX DMA",        [](Fixture& f) { f.dma_rx.Init.Mode = DMA_CIRCULAR; }},
		{"circular TX DMA",        [](Fixture& f) { f.dma_tx.Init.Mode = DMA_CIRCULAR; }},
		{"RX and TX share a channel", [](Fixture& f) { f.huart.hdmatx = &f.dma_rx; }},
		{"DMA Parent not linked",  [](Fixture& f) { f.dma_rx.Parent = nullptr; }},
		{"half-word memory width", [](Fixture& f) { f.dma_rx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD; }},
		{"half-word periph width", [](Fixture& f) { f.dma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD; }},
		{"peripheral increment on", [](Fixture& f) { f.dma_rx.Init.PeriphInc = DMA_PINC_ENABLE; }},
		{"memory increment off",   [](Fixture& f) { f.dma_tx.Init.MemInc = DMA_MINC_DISABLE; }},
		{"reversed RX direction",  [](Fixture& f) { f.dma_rx.Init.Direction = DMA_MEMORY_TO_PERIPH; }},
		{"DMA handle not READY",   [](Fixture& f) { f.dma_tx.State = HAL_DMA_STATE_RESET; }},
		{"8 bits WITH parity",     [](Fixture& f) { f.huart.Init.Parity = UART_PARITY_EVEN; }},
		{"RX-only mode",           [](Fixture& f) { f.huart.Init.Mode = UART_MODE_RX; }},
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
	checkNoViolations("no ownership violation across IDLE and TC");
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
	// The driver must keep retrying until the hardware really stops.
	f.loop();
	check(fake::model().rx_armed, "reception recovers once the abort succeeds");
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

void testTxTimeoutReportsFailureOnce()
{
	fake::reset();
	Fixture f;
	f.start();

	const uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 0};
	f.uart.send(std::span<const uint8_t>{frame, 8});
	fake::advance_tick(UART_ENGINE_TX_TIMEOUT_MARGIN_MS + 1000);
	f.loop();

	check(fake::model().tx_results.size() == 1 && !fake::model().tx_results[0],
	      "a stuck transfer yields exactly one terminal event, reporting failure");
	check(!f.uart.tx_busy(), "ownership of the frame is returned to the caller");
}

void testCtsDisablesTxDeadline()
{
	fake::reset();
	Fixture f;
	f.configure();
	f.huart.Init.HwFlowCtl = UART_HWCONTROL_RTS_CTS;
	check(f.uart.init(&f.huart), "CTS flow control is a valid configuration");
	f.uart.setTxHandler([](bool ok) { fake::model().tx_results.push_back(ok); });

	const uint8_t frame[2] = {9, 0};
	f.uart.send(std::span<const uint8_t>{frame, 2});
	fake::advance_tick(60000); // a peer may legitimately stall for a minute
	f.loop();
	f.loop();

	check(fake::model().tx_results.empty(),
	      "no deadline fires while CTS may legitimately hold the line");
	check(f.uart.tx_busy(), "the frame is still owned by the hardware");
}

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

} // namespace

int main()
{
	group("Initialization");
	testInitAcceptsValidConfig();
	testInitRefusalMatrix();
	testDuplicateAndRebind();

	group("RxOwnership");
	testRxIdleAndTc();
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
	testFailedArmDoesNotLeakSlot();
	testTxTimeoutReportsFailureOnce();
	testCtsDisablesTxDeadline();

	group("Watchdog");
	testWatchdogRevivesDeadReceiver();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
