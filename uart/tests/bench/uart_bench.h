/*
 * uart_bench.h — C-compatible half of the H7S hardware bench.
 *
 * The full-IRQ counters live here as a plain C struct (not uart_probe::Counter)
 * because they are written from stm32h7rsxx_it.c, which is C: the CubeMX
 * USER CODE sections around HAL_UART_IRQHandler / HAL_DMA_IRQHandler read
 * DWT->CYCCNT inline, with no function-call overhead inside the measurement.
 *
 * One writer context per counter (its own IRQ handler); readers copy under
 * PRIMASK. Same accounting discipline as uart_probe.h — no atomics.
 */

#ifndef UART_BENCH_H_
#define UART_BENCH_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	uint64_t total;
	uint32_t max;
	uint32_t calls;
} BenchCounter;

static inline void bench_counter_add(BenchCounter* c, uint32_t elapsed)
{
	c->total += elapsed;
	if (elapsed > c->max) { c->max = elapsed; }
	c->calls += 1u;
}

/* Written ONLY by the it.c handler each one wraps. */
extern BenchCounter g_bench_usart_irq;   /* USART3: RX IDLE + TX TC + errors  */
extern BenchCounter g_bench_rx_dma_irq;  /* GPDMA1_Channel11: RX TC path      */
extern BenchCounter g_bench_tx_dma_irq;  /* GPDMA1_Channel10: TX DMA complete */

/* Called from main.c USER CODE sections. */
void bench_init(void); /* enables I/D-cache + DWT, binds the driver to huart3 */
void bench_loop(void); /* uart.proceed() + TX generator + report transmission */

#ifdef __cplusplus
}
#endif

#endif /* UART_BENCH_H_ */
