/*
 * Portability smoke test: compiles the whole UART engine for a concrete
 * STM32 target, instantiating every template path and every API entry.
 * Compile-only (-c): HAL functions are declared by the family headers but
 * never linked. Built by build.sh for each target in this directory.
 */

#define UART_ENGINE_IMPLEMENT // emit the HAL callback definitions here
#include "Uart.h"

static UART_HandleTypeDef s_huart;
static DMA_HandleTypeDef  s_dma_rx;
static DMA_HandleTypeDef  s_dma_tx;

static Uart<256, 4> s_uart;

extern "C" int uart_port_test()
{
	s_huart.hdmarx = &s_dma_rx;
	s_huart.hdmatx = &s_dma_tx;

	bool ok = s_uart.init(&s_huart);

	s_uart.setRxHandler([](std::span<const uint8_t> bytes) { (void)bytes; });
	s_uart.setTxHandler([](bool tx_ok) { (void)tx_ok; });
	s_uart.setErrorHandler([](uint32_t code) { (void)code; });

	s_uart.proceed(0u);

	static const uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 0};
	ok = ok && s_uart.send(std::span<const uint8_t>{frame, sizeof frame});

	(void)s_uart.tx_busy();
	(void)s_uart.stats();
	return ok ? 0 : 1;
}
