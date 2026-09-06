/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Test fixture: a driver instance wired to the fake HAL, with a valid CubeMX-
 * shaped configuration that init() must accept, plus the handlers that record
 * what the application layer actually observes.
 */
#ifndef UART_TEST_FIXTURE_H_
#define UART_TEST_FIXTURE_H_

#include "fake_hal.h"
#include "Uart.h"

#include <string>

inline constexpr std::size_t kChunk  = 64;
inline constexpr std::size_t kChunks = 4;
using TestUart = Uart<kChunk, kChunks>;

struct Fixture {
	USART_TypeDef      usart{};
	DMA_Channel_TypeDef ch_rx{}, ch_tx{};
	DMA_HandleTypeDef  dma_rx{}, dma_tx{};
	UART_HandleTypeDef huart{};
	TestUart           uart{};

	// A configuration shaped exactly like the CubeMX output we validated
	// against the H7S target: everything init() demands, nothing it rejects.
	void configure() noexcept
	{
		huart.Instance = &usart;
		huart.Init.BaudRate = 115200;
		huart.Init.WordLength = UART_WORDLENGTH_8B;
		huart.Init.StopBits = UART_STOPBITS_1;
		huart.Init.Parity = UART_PARITY_NONE;
		huart.Init.Mode = UART_MODE_TX_RX;
		huart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
		huart.gState = HAL_UART_STATE_READY;
		huart.RxState = HAL_UART_STATE_READY;
		huart.ErrorCode = HAL_UART_ERROR_NONE;

		dma_rx.Instance = &ch_rx;
		dma_rx.State = HAL_DMA_STATE_READY;
		dma_rx.Parent = &huart;
		dma_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
		dma_rx.Init.PeriphInc = DMA_PINC_DISABLE;
		dma_rx.Init.MemInc = DMA_MINC_ENABLE;
		dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
		dma_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
		dma_rx.Init.Mode = DMA_NORMAL;

		dma_tx = dma_rx;
		dma_tx.Instance = &ch_tx;
		dma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;

		huart.hdmarx = &dma_rx;
		huart.hdmatx = &dma_tx;

		fake::model().huart = &huart;
	}

	bool start() noexcept
	{
		configure();
		if (!uart.init(&huart)) {
			return false;
		}
		uart.setRxHandler([](std::span<const uint8_t> bytes) {
			fake::note_consumer_sees(bytes.data());
			fake::model().rx_data.push_back(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
			fake::model().rx_events.push_back("data:" + std::to_string(bytes.size()));
			fake::note_consumer_done(bytes.data());
		});
		uart.setTxHandler([](bool ok) { fake::model().tx_results.push_back(ok); });
		uart.setErrorHandler([](uint32_t c) { fake::model().errors.push_back(c); });
		uart.setRxGapHandler([]() { fake::model().rx_events.push_back("gap"); });
		return true;
	}

	// One main-loop turn.
	void loop() noexcept { uart.proceed(fake::model().tick); }
};

// Joins the recorded event sequence so a test can assert on ordering, e.g.
// "data:3|gap|data:2".
inline std::string events() noexcept
{
	std::string out;
	for (const auto& e : fake::model().rx_events) {
		if (!out.empty()) { out += "|"; }
		out += e;
	}
	return out;
}

inline std::string rxText() noexcept
{
	std::string out;
	for (const auto& s : fake::model().rx_data) { out += s; }
	return out;
}

#endif /* UART_TEST_FIXTURE_H_ */
