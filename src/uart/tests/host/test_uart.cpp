/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

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

void testIdleAbortFailureKeepsRxClaimed()
{
	fake::reset();
	Fixture f;
	f.start();
	fake::rx_bytes("unsafe", 6);
	fake::model().fail_abort_receive = 100;
	fake::model().fail_dma_init = 100;
	fake::rx_idle_abort_failure();
	f.loop();
	check(rxText().empty() && events().empty(),
	      "READY UART with a failed IDLE DMA abort publishes neither data nor a gap yet");
	check(fake::model().rx_armed, "failed IDLE abort keeps the physical RX owner alive");
	checkNoViolations("failed IDLE abort never publishes or re-arms a DMA-owned slot");
	fake::model().fail_abort_receive = 0;
	fake::model().fail_dma_init = 0;
	f.loop();
	f.loop();
	check(events() == "gap" && rxText().empty(), "safe IDLE-abort recovery emits exactly one gap");
	fake::rx_bytes("ok", 2); fake::rx_idle(); f.loop();
	check(rxText() == "ok", "fresh RX works after the failed IDLE abort");
}

void testDmaErrorDoesNotReleaseTheOtherDirection()
{
	for (const bool rx_fault : {false, true}) {
		fake::reset();
		Fixture f;
		f.start();
		const std::array<uint8_t, 8> tx{};
		check(f.uart.send(tx), "cross-DMA fault starts with an owned TX span");
		fake::rx_bytes("partial", 7);
		if (rx_fault) { fake::model().fail_abort_transmit = 100; }
		else { fake::model().fail_abort_receive = 100; }
		fake::model().fail_dma_init = 100;
		fake::dma_error(rx_fault);
		if (rx_fault) {
			check(f.uart.tx_busy() && fake::model().tx_results.empty(),
			      "RX DMA error cannot return a span still borrowed by the TX DMA");
		} else {
			check(!f.uart.tx_busy() && fake::model().tx_results == std::vector<bool>{false},
			      "the actually stopped TX DMA reports one failure immediately");
		}
		f.loop();
		if (rx_fault) {
			check(f.uart.tx_busy() && fake::model().tx_armed && fake::model().tx_results.empty(),
			      "persistent TX repair failure preserves the borrow after an RX DMA error");
		} else {
			check(events().empty() && rxText().empty() && fake::model().rx_armed,
			      "TX DMA error cannot recycle the still-live RX buffer during failed repair");
		}
		fake::model().fail_abort_receive = 0;
		fake::model().fail_abort_transmit = 0;
		fake::model().fail_dma_init = 0;
		f.loop(); f.loop();
		check(!f.uart.tx_busy() && fake::model().tx_results == std::vector<bool>{false},
		      "repair returns TX exactly once and never reports synthetic success for a fault");
		check(events() == "gap" && rxText().empty() && fake::model().rx_armed,
		      "cross-DMA repair discards one RX prefix and rearms safely");
		checkNoViolations("cross-DMA fault preserves both physical owners");
	}
}

void testDmaFaultOutranksLateTcEvenWithCts()
{
	fake::reset();
	Fixture f;
	f.start();
	f.huart.Init.HwFlowCtl = UART_HWCONTROL_CTS;
	check(f.uart.setBaudRate(115200u), "CTS fault test applies the disabled-stall policy");
	f.loop();
	const std::array<uint8_t, 8> tx{};
	check(f.uart.send(tx), "CTS fault test starts TX");
	fake::dma_error(true);
	fake::tx_done(); // the independent TX hardware could finish before the loop runs
	check(f.uart.tx_busy() && fake::model().tx_results.empty(),
	      "a late TC cannot steal a DMA error's deferred terminal verdict");
	f.loop();
	check(!f.uart.tx_busy() && fake::model().tx_results == std::vector<bool>{false},
	      "CTS cannot suppress explicit fault recovery or turn the failure into success");
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

// ---- WakeHandler: "proceed() has work", raised from ISR context ----------

struct WakeCount {
	unsigned n = 0;
	void bump() noexcept { ++n; }
};

// rx_progress(): what DMA has taken into the chunk it still owns, read from
// the counter in thread context, zero whenever there is no such chunk.
void testRxProgressIsASnapshotOfTheActiveChunk()
{
	fake::reset();
	Fixture f;
	check(f.uart.rx_progress() == 0, "0 before init: no handle, nothing armed");
	f.start();
	check(f.uart.rx_progress() == 0, "0 right after arming: the chunk is empty");

	fake::rx_bytes("abc", 3);
	check(f.uart.rx_progress() == 3, "3 bytes taken by DMA, no event yet: 3");
	fake::rx_bytes("de", 2);
	check(f.uart.rx_progress() == 5, "the snapshot follows the counter: 5");
	fake::rx_half();
	check(f.uart.rx_progress() == 5, "the ignored half-transfer event changes nothing");
	fake::rx_idle();
	check(f.uart.rx_progress() == 0, "IDLE published the chunk and re-armed a fresh one: 0 again");
	f.loop();
	check(rxText() == "abcde", "the published bytes are exactly the ones the snapshot counted");

	std::string full(kChunk, 'x');
	fake::rx_bytes(full.data(), full.size() - 1u);
	check(f.uart.rx_progress() == kChunk - 1u, "one byte short of a full chunk");
	fake::rx_bytes("y", 1);
	fake::rx_tc();
	check(f.uart.rx_progress() == 0, "TC published the full chunk: 0");
	f.loop();

	fake::rx_bytes("zz", 2);
	check(f.uart.rx_progress() == 2, "2 before the error");
	fake::rx_error(HAL_UART_ERROR_ORE);
	check(f.uart.rx_progress() == 0, "an RX error stops reception until proceed() repairs it: 0, not a stale count");
	f.loop();
	check(f.uart.rx_progress() == 0 && events().find("gap") != std::string::npos,
	      "re-armed after recovery with an empty chunk, the gap delivered");
	fake::rx_bytes("ok", 2);
	check(f.uart.rx_progress() == 2, "and counting again");
	fake::rx_idle();
	f.loop();
	checkNoViolations("reading the counter touched no ownership");
}

void testWakeFollowsEveryIsrEventWithWork()
{
	fake::reset();
	Fixture f;
	f.start();
	WakeCount wakes;
	f.uart.setWakeHandler(TestUart::WakeHandler{tiny::bind<&WakeCount::bump>(wakes)});

	fake::rx_bytes("abc", 3);
	fake::rx_idle();
	check(wakes.n == 1, "an IDLE-published chunk raises exactly one wake, before proceed() runs");
	f.loop();
	check(wakes.n == 1 && rxText() == "abc", "proceed() itself raises none and delivers the chunk");

	std::string full(kChunk, 'x');
	fake::rx_bytes(full.data(), full.size());
	fake::rx_tc();
	check(wakes.n == 2, "a TC-published chunk raises one wake");
	f.loop();

	fake::rx_bytes("q", 1);
	fake::rx_half();
	check(wakes.n == 2, "a half-transfer event, which the driver ignores, raises none");
	fake::rx_idle();
	check(wakes.n == 3, "the IDLE that ends that chunk does");
	f.loop();

	const uint8_t frame[4] = {1, 2, 3, 0};
	check(f.uart.send(std::span<const uint8_t>{frame, 4}), "send starts a transfer");
	check(wakes.n == 3, "starting a transmission raises none: the caller is awake");
	fake::tx_done();
	check(wakes.n == 4 && !f.uart.tx_busy(), "TX completion raises one wake, after ownership returned");

	fake::rx_bytes("zz", 2);
	fake::rx_error(HAL_UART_ERROR_ORE);
	check(wakes.n == 5, "an RX error raises one wake: recovery is proceed()'s job");
	f.loop();
	check(events().find("gap") != std::string::npos, "and the ordered gap marker it queued is delivered");

	checkNoViolations("wake handling changed no ownership");
}

void testWakeIsOptionalAndReplaceable()
{
	fake::reset();
	Fixture f;
	f.start();
	// No handler: every event path must stay a null test.
	fake::rx_bytes("abc", 3);
	fake::rx_idle();
	fake::tx_done();
	fake::rx_error(HAL_UART_ERROR_FE);
	f.loop();
	check(rxText() == "abc", "unset wake handler: the driver runs exactly as before");

	WakeCount first;
	WakeCount second;
	f.uart.setWakeHandler(TestUart::WakeHandler{tiny::bind<&WakeCount::bump>(first)});
	fake::rx_bytes("d", 1);
	fake::rx_idle();
	f.uart.setWakeHandler(TestUart::WakeHandler{tiny::bind<&WakeCount::bump>(second)});
	fake::rx_bytes("e", 1);
	fake::rx_idle();
	f.uart.setWakeHandler(TestUart::WakeHandler{});
	fake::rx_bytes("f", 1);
	fake::rx_idle();
	f.loop();
	check(first.n == 1 && second.n == 1 && rxText() == "abcdef",
	      "the handler is replaceable and removable while the engine runs");
	checkNoViolations("no ownership violation");
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

void testWatchdogChecksHardwareDespiteBusyHalState()
{
	for (unsigned fault = 0u; fault < 3u; ++fault) {
		fake::reset();
		Fixture f;
		check(f.start(), "hardware RX watchdog fixture starts");
		if (fault == 0u) {
			const std::array<uint8_t, kChunk> bytes{};
			fake::rx_bytes(bytes.data(), bytes.size()); // hardware finished; no DMA IRQ
		} else if (fault == 1u) {
			f.usart.CR3 &= ~USART_CR3_DMAR; // request stopped without updating HAL state
		} else {
			f.dma_rx.CountRemaining = static_cast<uint32_t>(kChunk + 1u);
		}
		check(f.huart.RxState == HAL_UART_STATE_BUSY_RX && f.dma_rx.State == HAL_DMA_STATE_BUSY,
		      "software state alone still claims an active receiver");
		audits(f, UART_ENGINE_FAIL_THRESHOLD + 1);
		check(fake::model().rx_armed && f.dma_rx.CountRemaining == kChunk &&
		      (f.usart.CR3 & USART_CR3_DMAR) != 0u && f.uart.stats().restarts > 1u,
		      "exhausted, disabled or impossible RX hardware recovers after debounce");
		f.loop(); // drain the ordered discontinuity marker
		check(events() == "gap", "untrusted old chunk becomes one gap, never fabricated data");
		fake::rx_bytes("OK", 2u);
		fake::rx_idle();
		f.loop();
		check(events() == "gap|data:2" && rxText() == "OK", "recovery permits the next real frame");
		checkNoViolations("watchdog recovery retains ownership until DMA is stopped");
	}
}

void testRxWatchdogAllowsIdleAndDelayedCompletion()
{
	fake::reset();
	Fixture f;
	check(f.start(), "idle RX watchdog fixture starts");
	const uint32_t restarts = f.uart.stats().restarts;
	audits(f, 4 * UART_ENGINE_FAIL_THRESHOLD);
	check(f.uart.stats().restarts == restarts && events().empty(), "an idle armed receiver does not time out");
	const std::array<uint8_t, kChunk> bytes{};
	fake::rx_bytes(bytes.data(), bytes.size());
	audits(f, 1); // one bad observation, completion is merely delayed
	fake::rx_tc();
	f.loop();
	audits(f, UART_ENGINE_FAIL_THRESHOLD);
	check(f.uart.stats().restarts == restarts && rxText().size() == kChunk &&
	      events() == "data:64", "a completion inside the debounce window is delivered without a restart");
	checkNoViolations("delayed completion preserves ownership");
}

void testTortureGeneratorRespectsTheLastRxByte()
{
	bool valid = true;
	for (uint32_t seed = 1u; seed <= 256u; ++seed) {
		fake::reset();
		torture::State state;
		if (!state.f.start()) { valid = false; break; }
		const std::array<uint8_t, kChunk - 1u> bytes{};
		fake::rx_bytes(bytes.data(), bytes.size());
		torture::Rng rng{seed};
		torture::step(state, rng);
		valid = valid && fake::model().violations.empty();
	}
	check(valid, "the generator never forces a two-byte DMA write into one remaining byte");
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
	testIdleAbortFailureKeepsRxClaimed();
	testDmaErrorDoesNotReleaseTheOtherDirection();
	testDmaFaultOutranksLateTcEvenWithCts();
	testProceedIsSafeAgainstHandlerReentry();
	testDmaBufferNeverVisibleToConsumer();
	testSlotConservation();

	group("RxDiscontinuity");
	testOverflowProducesOrderedGap();
	testRxErrorProducesGapBeforeNextData();

	group("TxOwnership");
	testTxSingleTerminalEvent();
	testTxRefusesOversizedFrame();

	group("WakeHandler");
	testWakeFollowsEveryIsrEventWithWork();
	testWakeIsOptionalAndReplaceable();

	group("RxProgress");
	testRxProgressIsASnapshotOfTheActiveChunk();

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
	testWatchdogChecksHardwareDespiteBusyHalState();
	testRxWatchdogAllowsIdleAndDelayedCompletion();

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
	testTortureGeneratorRespectsTheLastRxByte();
	testRandomizedTorture(steps);

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
