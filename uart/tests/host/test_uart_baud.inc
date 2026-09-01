/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * setBaudRate(): reconfiguring the line speed of a running engine.
 * Included by test_uart.cpp.
 *
 * The guarantees are about what the layer above must be able to rely on: the
 * frame in flight is never corrupted, the stream is never silently spliced
 * across the speed change, and a request the hardware cannot satisfy costs a
 * gap rather than the link.
 */

// Reprogramming BRR under a live DMA transfer would corrupt the frame being
// shifted out, so a transmission in progress must simply refuse the change.
void testBaudRefusedWhileTransmitting()
{
	fake::reset();
	Fixture f;
	f.start();

	const uint8_t frame[4] = {1, 2, 3, 0};
	f.uart.send(std::span<const uint8_t>{frame, 4});

	check(!f.uart.setBaudRate(1000000u), "a speed change is refused while a frame is in flight");
	check(f.huart.Init.BaudRate == 115200u, "and the configured rate is left untouched");
	check(fake::model().init_calls == 0u, "the peripheral was never reconfigured");
	check(fake::model().tx_armed, "the transfer is still running");

	// Once the line is free the same request succeeds.
	fake::tx_done();
	check(f.uart.setBaudRate(1000000u), "and succeeds once the frame has completed");
	check(fake::model().applied_baud == 1000000u, "the peripheral now runs at the new rate");
}

// Bytes received at the old speed must never be spliced onto the new stream:
// the decoder above has to see the discontinuity, in the right place.
void testBaudChangeReArmsAndGaps()
{
	fake::reset();
	Fixture f;
	f.start();

	fake::rx_bytes("old", 3);
	fake::rx_idle();
	fake::rx_bytes("part", 4); // still in the DMA's chunk when the speed changes

	check(f.uart.setBaudRate(3000000u), "the speed change succeeds");
	check(fake::model().rx_armed, "reception is armed again afterwards");
	check(f.huart.Init.BaudRate == 3000000u, "the handle carries the new rate");

	fake::rx_bytes("new", 3);
	fake::rx_idle();
	f.loop();

	const std::string seq = events();
	const std::string expected = "data:3|gap|data:3";
	check(seq == expected,
	      "the truncated chunk becomes a gap between the two speeds (got " + seq + ")");
	checkNoViolations("no ownership violation across the reconfiguration");
}

// HAL_UART_Init drops FIFOEN and both threshold fields while huart->FifoMode
// keeps claiming the mode is on. The driver saves the truth from the
// registers and must put the hardware back in agreement.
void testBaudChangePreservesFifoConfiguration()
{
	fake::reset();
	Fixture f;
	f.configure();
	// A FIFO-enabled peripheral, as CubeMX leaves it.
	f.usart.CR1 |= USART_CR1_FIFOEN;
	f.usart.CR3 |= (2U << 29) | (3U << 25); // non-default TX/RX thresholds
	f.huart.FifoMode = UART_FIFOMODE_ENABLE;
	check(f.uart.init(&f.huart), "the engine starts on a FIFO-enabled peripheral");

	const uint32_t tx_thr = f.usart.CR3 & USART_CR3_TXFTCFG;
	const uint32_t rx_thr = f.usart.CR3 & USART_CR3_RXFTCFG;

	check(f.uart.setBaudRate(2000000u), "the speed change succeeds");
	check((f.usart.CR1 & USART_CR1_FIFOEN) != 0u,
	      "FIFO mode survives the reconfiguration");
	check((f.usart.CR3 & USART_CR3_TXFTCFG) == tx_thr, "the TX threshold survives");
	check((f.usart.CR3 & USART_CR3_RXFTCFG) == rx_thr, "the RX threshold survives");
}

void testFailedFifoRestoreRollsBackTheWholeChange()
{
	fake::reset();
	Fixture f;
	f.configure();
	f.usart.CR1 |= USART_CR1_FIFOEN;
	f.usart.CR3 |= (2U << 29) | (3U << 25);
	f.huart.FifoMode = UART_FIFOMODE_ENABLE;
	check(f.uart.init(&f.huart), "FIFO rollback fixture starts");
	fake::model().fail_fifo_config = 1;

	check(!f.uart.setBaudRate(2000000u), "a failed FIFO restore fails the speed change");
	check(f.huart.Init.BaudRate == 115200u && fake::model().applied_baud == 115200u,
	      "the previous baud is restored together with FIFO state");
	check((f.usart.CR1 & USART_CR1_FIFOEN) != 0u,
	      "FIFO mode is restored after the failed attempt");
	check(fake::model().rx_armed, "reception is live after transactional rollback");
}

void testBaudChangeRefreshesCtsWatchPolicy()
{
	fake::reset();
	Fixture f;
	f.start();
	f.huart.Init.HwFlowCtl = UART_HWCONTROL_RTS_CTS;
	check(f.uart.setBaudRate(1000000u), "baud change can enable CTS flow control");

	static std::vector<uint8_t> frame(1024, 0xA5);
	f.uart.send(std::span<const uint8_t>{frame.data(), frame.size()});
	audits(f, 4 * UART_ENGINE_FAIL_THRESHOLD);
	check(f.uart.tx_busy(), "newly enabled CTS disables false TX-stall recovery");
	check(fake::model().tx_results.empty(), "CTS hold produces no terminal event");
}

void testBaudChangeCanReenableStallDetection()
{
	fake::reset();
	Fixture f;
	f.configure();
	f.huart.Init.HwFlowCtl = UART_HWCONTROL_RTS_CTS;
	check(f.uart.init(&f.huart), "CTS-to-unflowed fixture starts");
	f.uart.setTxHandler([](bool ok) noexcept { fake::model().tx_results.push_back(ok); });
	f.huart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	check(f.uart.setBaudRate(1000000u), "baud change can disable CTS flow control");

	static std::vector<uint8_t> frame(1024, 0x5A);
	f.uart.send(std::span<const uint8_t>{frame.data(), frame.size()});
	audits(f, UART_ENGINE_FAIL_THRESHOLD + 2);
	check(!f.uart.tx_busy(), "stall detection is re-enabled after CTS is removed");
	check(fake::model().tx_results.size() == 1u && !fake::model().tx_results[0],
	      "the frozen transfer reports one failure");
}

// A rate this kernel clock cannot produce leaves the peripheral half
// configured and disabled. The engine must fall back to the speed that
// worked instead of losing the link.
void testUnreachableBaudKeepsTheLinkAlive()
{
	fake::reset();
	Fixture f;
	f.start();
	fake::model().max_baud = 6000000u;

	check(!f.uart.setBaudRate(12000000u), "an unreachable rate is reported as failure");
	check(f.huart.Init.BaudRate == 115200u, "the previous rate is put back");
	check(fake::model().applied_baud == 115200u, "and really re-applied to the peripheral");
	check(fake::model().rx_armed, "reception is running again");

	// The link genuinely still works.
	fake::rx_bytes("alive", 5);
	fake::rx_idle();
	f.loop();
	check(events().find("data:5") != std::string::npos, "and still delivers data");
}

// The engine must tolerate the call in states where it cannot act.
void testBaudChangeRejectsNonsense()
{
	fake::reset();
	Fixture f;
	f.configure();

	TestUart unbound;
	check(!unbound.setBaudRate(115200u), "an unbound engine refuses the request");

	f.uart.init(&f.huart);
	check(!f.uart.setBaudRate(0u), "a zero rate is refused");
	check(fake::model().init_calls == 0u, "neither attempt reconfigured anything");

	f.huart.Init.WordLength = UART_WORDLENGTH_7B;
	check(!f.uart.setBaudRate(1000000u), "invalid byte framing is refused before teardown");
	check(fake::model().rx_armed, "invalid framing leaves the working receiver untouched");
}
