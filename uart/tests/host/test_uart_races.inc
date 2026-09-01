/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Deterministic cross-direction and failure cases. Included by test_uart.cpp.
 *
 * Note what is deliberately NOT here: "TxHandler calls send() again". The
 * handler runs from an ISR on a normal completion and from thread context on a
 * watchdog resolution, so cementing a re-entrant send as a tested guarantee
 * would quietly make it part of the API. COBS does not need it.
 */

/* ====================== Cross-direction arbitration ===================== */

// A genuine RX error arriving while the TX side is being torn down must be
// handled on its own merits — and must not let the TX terminal event be
// claimed twice.
void testRxErrorDuringTxTeardown()
{
	fake::reset();
	Fixture f;
	f.start();

	const uint8_t frame[4] = {1, 2, 3, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});

	// Inject the RX error from inside HAL_UART_AbortTransmit itself: the TX
	// teardown gate is already raised, while the RX gate is deliberately not.
	fake::model().rx_error_inside_abort_transmit = true;
	fake::rx_bytes("half", 4);

	fake::tx_progress(0); // counter never moves -> stall
	for (int i = 0; i < UART_ENGINE_FAIL_THRESHOLD + 2; ++i) {
		fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1);
		f.loop();
	}

	check(fake::model().tx_results.size() == 1 && !fake::model().tx_results[0],
	      "exactly one failed TX terminal event despite the overlapping RX error");
	check(!f.uart.tx_busy(), "TX ownership was returned");
	check(fake::model().rx_armed, "reception recovered from its own error");
	const std::string seq = events();
	check(seq.find("gap") != std::string::npos,
	      "the discarded RX chunk still produced a gap (got " + seq + ")");
}

// Mirror: a TX/DMA error while the receiver is being restarted.
void testTxErrorDuringRxTeardown()
{
	fake::reset();
	Fixture f;
	f.start();

	const uint8_t frame[4] = {4, 5, 6, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});

	// Inject the TX failure from inside HAL_UART_AbortReceive itself: the RX
	// teardown gate is already raised, while the TX gate is deliberately not.
	fake::model().tx_error_inside_abort_receive = true;
	fake::rx_bytes("xy", 2);
	fake::rx_error(HAL_UART_ERROR_DMA); // forces an RX teardown
	f.loop();
	f.loop();

	check(fake::model().tx_results.size() == 1 && !fake::model().tx_results[0],
	      "exactly one failed TX terminal event despite the overlapping RX teardown");
	check(fake::model().rx_armed, "the receiver came back");
	checkNoViolations("no ownership violation across crossed teardowns");
}

/* =========================== Failure recovery =========================== */

void testInitFailsWhenFirstArmFails()
{
	fake::reset();
	Fixture f;
	f.configure();
	fake::model().fail_arm = 1; // the very first ReceiveToIdle_DMA fails

	check(!f.uart.init(&f.huart), "init reports failure when the hardware will not start");
	check(f.uart.instance() == nullptr, "and leaves the object completely unbound");

	// The registry entry must have been released too, or a retry cannot bind.
	fake::model().fail_arm = 0;
	check(f.uart.init(&f.huart), "the same object can be initialized again after the failure");
	check(fake::model().rx_armed, "and reception really starts the second time");
}

void testSendFailureLeavesNoTrace()
{
	fake::reset();
	Fixture f;
	f.start();

	// Make the HAL refuse the transfer by leaving a phantom one in flight.
	fake::model().tx_armed = true;
	const uint8_t frame[4] = {7, 8, 9, 0};
	check(!f.uart.send(std::span<const uint8_t>{frame, 4}), "a refused send returns false");
	check(!f.uart.tx_busy(), "and does not claim the caller's buffer");
	check(fake::model().tx_results.empty(), "no terminal event is invented for a frame never sent");

	fake::model().tx_armed = false;
	fake::model().violations.clear(); // the phantom transfer is a test artefact
	check(f.uart.send(std::span<const uint8_t>{frame, 4}), "the next send works normally");
	fake::tx_done();
	check(fake::model().tx_results.size() == 1 && fake::model().tx_results[0],
	      "and completes with exactly one success");
}

// DMA has fed the UART while its shift register/FIFO are still draining.
// A normal bounded tail is progress-complete, not a stall.
void testDrainingUartWithinBudgetIsNotAStall()
{
	fake::reset();
	Fixture f;
	f.start();

	const uint8_t frame[4] = {1, 2, 3, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});
	fake::tx_dma_done(); // counter at zero, TC still clear

	for (int i = 0; i < 2; ++i) {
		fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1);
		f.loop();
	}
	check(f.uart.tx_busy(), "a UART tail inside its drain budget is not called stalled");
	check(fake::model().tx_results.empty(), "no terminal event while the line is still busy");

	fake::tx_uart_tc();
	check(!f.uart.tx_busy(), "and it completes normally once TC arrives");
	check(fake::model().tx_results.size() == 1 && fake::model().tx_results[0],
	      "reported once, as success");
}

// The synthetic completion and the real ISR race for the same terminal event.
void testSyntheticAndRealCompletionRaceOnce()
{
	fake::reset();
	Fixture f;
	f.start();

	const uint8_t frame[4] = {1, 2, 3, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});
	fake::tx_dma_done();
	f.huart.Instance->ISR |= USART_ISR_TC;      // looks physically complete...
	fake::model().tx_cplt_inside_abort = true;  // ...and the real ISR fires inside the abort

	fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1);
	f.loop();

	check(fake::model().tx_results.size() == 1,
	      "exactly one terminal event when both paths claim the same completion");
	check(fake::model().tx_results[0], "reported as success");
	check(!f.uart.tx_busy(), "ownership returned exactly once");
}

// The stall watchdog wants to reclaim the frame, but the abort itself times
// out: the DMA may still be reading the caller's buffer, so ownership must
// NOT be returned. A later audit, once the abort succeeds, may recover.
void testAbortTimeoutDuringStallKeepsOwnership()
{
	fake::reset();
	Fixture f;
	f.start();

	static std::vector<uint8_t> big(4096, 0x33);
	f.uart.send(std::span<const uint8_t>{big.data(), big.size()});
	fake::tx_progress(64);                     // moves once...
	fake::model().fail_abort_transmit = 100;   // ...and every abort will time out
	fake::model().fail_dma_init = 100;         // ...and hardware reset cannot recover it yet

	for (int i = 0; i < 4 * UART_ENGINE_FAIL_THRESHOLD; ++i) {
		fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1);
		f.loop();
	}
	check(f.uart.tx_busy(),
	      "a failed abort never returns ownership — the DMA may still be reading");
	check(fake::model().tx_results.empty(), "and no terminal event is reported yet");
	check(fake::model().tx_armed, "the fake DMA confirms it still holds the buffer");

	// Once the hardware finally lets go, the next audit resolves it.
	fake::model().fail_abort_transmit = 0;
	fake::model().fail_dma_init = 0;
	for (int i = 0; i < UART_ENGINE_FAIL_THRESHOLD + 2; ++i) {
		fake::advance_tick(UART_ENGINE_CHECK_PERIOD_MS + 1);
		f.loop();
	}
	check(!f.uart.tx_busy(), "recovery completes once the abort succeeds");
	check(fake::model().tx_results.size() == 1 && !fake::model().tx_results[0],
	      "reported exactly once, as failure");
}

void testFullRecoveryRepairsBothDmaDirections()
{
	fake::reset();
	Fixture f;
	f.start();
	const uint8_t frame[4] = {1, 2, 3, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});
	fake::model().fail_abort_transmit = 1;
	fake::model().fail_abort_receive = 1;
	f.huart.ErrorCode = HAL_UART_ERROR_DMA;

	audits(f, UART_ENGINE_FAIL_THRESHOLD + 1);
	check(fake::model().dma_init_calls == 2u,
	      "full recovery repairs TX and RX after sequential abort timeouts");
	check(!f.uart.tx_busy() && fake::model().tx_results.size() == 1u &&
	      !fake::model().tx_results[0],
	      "the interrupted TX borrow receives exactly one failure");
	check(fake::model().rx_armed, "RX is armed after both DMA directions recover");
	checkNoViolations("two-direction DMA repair preserves every buffer owner");
}
