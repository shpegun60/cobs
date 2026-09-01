/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * uart_bench.cpp — hardware bench for uart/Uart.h on STM32H7S3L8.
 * Canonical copy: uart/tests/bench/ in the cobs repository; the file in the
 * Cube project (Boot/Core/Src) is a copy of this one.
 *
 * The PC (uart/tests/bench/bench.py) drives everything over USART3 (the
 * ST-LINK VCP). Single-byte commands, delivered alone after a line pause so
 * they arrive as their own 1-byte chunk:
 *
 *   'R'  reset the measurement window (all counters zeroed under PRIMASK)
 *   'S'  stop: freeze the window and transmit a one-line report
 *   'T'  start the TX generator (64-byte frames back-to-back from the loop)
 *   't'  stop the TX generator
 *   '1'  change the live link to 115200 baud from thread context
 *   '3'  change the live link to 3 Mbaud from thread context
 *
 * Any other traffic is scenario payload; the PC generator avoids all six
 * current command bytes in its payload pattern.
 *
 * CPU accounting (see doc: the hardware IRQ counters already CONTAIN the
 * library's Rx callback, so probe.rx is NOT added to the total — it exists
 * only for decomposition against usart_irq / rx_dma_irq):
 *
 *   UART cycles = Δusart_irq + Δrx_dma_irq + Δtx_dma_irq
 *               + Δprobe.slow + Δprobe.tx_start
 */

#define UART_ENGINE_PROBE 1
#define UART_ENGINE_IMPLEMENT // HAL callback definitions live in this TU
#include "Uart.h"

#include "uart_bench.h"
#include "usart.h" // huart3

#include <cstdio>
#include <cstring>

/* Chunk geometry of the run: rebuild with -DBENCH_CHUNK_SIZE=128/256/512 for
 * the sweep. The defaults mirror the library's own, so a plain build measures
 * exactly the configuration an application gets from Uart<>. */
#ifndef BENCH_CHUNK_SIZE
#define BENCH_CHUNK_SIZE 128u
#endif
#ifndef BENCH_CHUNK_COUNT
#define BENCH_CHUNK_COUNT 8u
#endif
/* High-baud sweep: rebuild with -DBENCH_BAUD=1000000/3000000/6000000/10000000.
 * USART3 kernel clock is PCLK1 = 150 MHz: oversampling 16 tops out at
 * 9.375 Mbaud, so faster rates switch to oversampling 8 (max 18.75M). */
#ifndef BENCH_BAUD
#define BENCH_BAUD 115200u
#endif

BenchCounter g_bench_usart_irq;
BenchCounter g_bench_rx_dma_irq;
BenchCounter g_bench_tx_dma_irq;

namespace {

/* Default .bss = AXI SRAM @0x24000000: GPDMA-reachable AND cacheable, so the
 * driver's real D-cache maintenance path is exercised. No .dma section games:
 * placement stays the application's decision, and this application's default
 * placement is already correct. */
Uart<BENCH_CHUNK_SIZE, BENCH_CHUNK_COUNT> s_uart;

struct Window {
	uint32_t start_ms = 0u;
	uint32_t rx_bytes = 0u;
	uint32_t rx_chunks = 0u;
	uint32_t gaps = 0u;
	uint32_t tx_frames = 0u;
	/* uart.stats() has no reset — keep window baselines. */
	uint32_t ovr0 = 0u, rxe0 = 0u, txe0 = 0u, rst0 = 0u;
};

/* Frozen at 'S' so the report covers exactly the window, not the cycles
 * spent formatting and transmitting it. */
struct Frozen {
	BenchCounter usart, rx_dma, tx_dma;
	uart_probe::Stats probe;
	Window win;
	uint32_t win_ms = 0u;
	uint32_t ovr = 0u, rxe = 0u, txe = 0u, rst = 0u;
};

Window s_win;
Frozen s_frozen;
volatile bool s_report_pending = false;
volatile uint32_t s_pending_baud = 0u;
bool s_tx_gen = false;

/* 0x55 pattern, no '\n': the PC reader separates report lines from generator
 * frames by the line break alone. */
uint8_t s_tx_frame[64];
char s_report[384];

uint32_t sat32(const uint64_t v)
{
	return (v > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(v);
}

uint32_t avg(const BenchCounter& c)
{
	return (c.calls != 0u) ? static_cast<uint32_t>(c.total / c.calls) : 0u;
}

uint32_t avgp(const uart_probe::Counter& c)
{
	return (c.calls != 0u) ? static_cast<uint32_t>(c.total / c.calls) : 0u;
}

void resetWindow()
{
	const auto& st = s_uart.stats();
	s_win = Window{};
	s_win.start_ms = HAL_GetTick();
	s_win.ovr0 = st.rx_overrun;
	s_win.rxe0 = st.rx_errors;
	s_win.txe0 = st.tx_errors;
	s_win.rst0 = st.restarts;

	/* Counters have one writer each (an ISR); zeroing them from thread
	 * context needs the same PRIMASK window a snapshot does. */
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	g_bench_usart_irq  = BenchCounter{};
	g_bench_rx_dma_irq = BenchCounter{};
	g_bench_tx_dma_irq = BenchCounter{};
	uart_probe::g_stats = uart_probe::Stats{};
	__set_PRIMASK(primask);
}

void freezeWindow()
{
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	s_frozen.usart  = g_bench_usart_irq;
	s_frozen.rx_dma = g_bench_rx_dma_irq;
	s_frozen.tx_dma = g_bench_tx_dma_irq;
	s_frozen.probe  = uart_probe::g_stats;
	__set_PRIMASK(primask);

	const auto& st = s_uart.stats();
	s_frozen.win = s_win;
	s_frozen.win_ms = HAL_GetTick() - s_win.start_ms;
	s_frozen.ovr = st.rx_overrun - s_win.ovr0;
	s_frozen.rxe = st.rx_errors  - s_win.rxe0;
	s_frozen.txe = st.tx_errors  - s_win.txe0;
	s_frozen.rst = st.restarts   - s_win.rst0;
}

void buildReport()
{
	const Frozen& f = s_frozen;

	/* Total UART CPU: three full-IRQ counters + the two thread-context
	 * library sites. probe.rx is already inside usart/rx_dma. */
	const uint64_t cycles = f.usart.total + f.rx_dma.total + f.tx_dma.total
	                      + f.probe.slow.total + f.probe.tx_start.total;
	/* Milli-percent of one core: at these clock rates two decimal places
	 * round everything interesting down to 0.00. */
	const uint64_t denom =
		static_cast<uint64_t>(SystemCoreClock / 1000u) * (f.win_ms ? f.win_ms : 1u);
	const uint32_t cpu_mp = static_cast<uint32_t>((cycles * 100000u) / denom);

	std::snprintf(
		s_report, sizeof s_report,
		"\r\nRX=%lucy/%lu avg=%lu max=%lu | "
		"USART=%lucy/%lu avg=%lu max=%lu | "
		"RXDMA=%lucy/%lu avg=%lu max=%lu | "
		"TXDMA=%lucy/%lu avg=%lu max=%lu | "
		"SLOW=%lucy/%lu avg=%lu max=%lu | "
		"TXSTART=%lucy/%lu avg=%lu max=%lu | "
		"BYTES=%lu CH=%lu GAP=%lu TXFR=%lu | "
		"WIN=%lums CPU=%lu.%03lu%% | OVR=%lu ERR=%lu RST=%lu\r\n",
		static_cast<unsigned long>(sat32(f.probe.rx.total)),
		static_cast<unsigned long>(f.probe.rx.calls),
		static_cast<unsigned long>(avgp(f.probe.rx)),
		static_cast<unsigned long>(f.probe.rx.max),
		static_cast<unsigned long>(sat32(f.usart.total)),
		static_cast<unsigned long>(f.usart.calls),
		static_cast<unsigned long>(avg(f.usart)),
		static_cast<unsigned long>(f.usart.max),
		static_cast<unsigned long>(sat32(f.rx_dma.total)),
		static_cast<unsigned long>(f.rx_dma.calls),
		static_cast<unsigned long>(avg(f.rx_dma)),
		static_cast<unsigned long>(f.rx_dma.max),
		static_cast<unsigned long>(sat32(f.tx_dma.total)),
		static_cast<unsigned long>(f.tx_dma.calls),
		static_cast<unsigned long>(avg(f.tx_dma)),
		static_cast<unsigned long>(f.tx_dma.max),
		static_cast<unsigned long>(sat32(f.probe.slow.total)),
		static_cast<unsigned long>(f.probe.slow.calls),
		static_cast<unsigned long>(avgp(f.probe.slow)),
		static_cast<unsigned long>(f.probe.slow.max),
		static_cast<unsigned long>(sat32(f.probe.tx_start.total)),
		static_cast<unsigned long>(f.probe.tx_start.calls),
		static_cast<unsigned long>(avgp(f.probe.tx_start)),
		static_cast<unsigned long>(f.probe.tx_start.max),
		static_cast<unsigned long>(f.win.rx_bytes),
		static_cast<unsigned long>(f.win.rx_chunks),
		static_cast<unsigned long>(f.win.gaps),
		static_cast<unsigned long>(f.win.tx_frames),
		static_cast<unsigned long>(f.win_ms),
		static_cast<unsigned long>(cpu_mp / 1000u),
		static_cast<unsigned long>(cpu_mp % 1000u),
		static_cast<unsigned long>(f.ovr),
		static_cast<unsigned long>(f.rxe + f.txe),
		static_cast<unsigned long>(f.rst));
}

void onRx(std::span<const uint8_t> bytes)
{
	/* Commands arrive as their own 1-byte chunk (the PC pauses the line
	 * around them, so IDLE flushes them alone). */
	if (bytes.size() == 1u) {
		switch (bytes[0]) {
		case 'R': resetWindow();            return;
		case 'S': freezeWindow();
		          s_report_pending = true;  return;
		case 'T': s_tx_gen = true;          return;
		case 't': s_tx_gen = false;         return;
		/* Live speed change. Deferred to the loop: this runs in the ISR,
		 * setBaudRate() is thread-context only (it aborts and re-inits). */
		case '1': s_pending_baud = 115200u;  return;
		case '3': s_pending_baud = 3000000u; return;
		default: break;
		}
	}
	s_win.rx_bytes += static_cast<uint32_t>(bytes.size());
	s_win.rx_chunks += 1u;
}

} // namespace

extern "C" void bench_init(void)
{
	/* The first baseline runs with caches ON: AXI SRAM is cacheable, so the
	 * driver's AN4839 maintenance (invalidate on RX, clean on TX) is real. */
	SCB_EnableICache();
	SCB_EnableDCache();

	uart_probe::init(); // TRCENA + CM7 CoreSight unlock + CYCCNTENA

	/* Sweep support: rebase the CubeMX-configured UART onto the bench baud.
	 * HAL_UART_Init rewrites CR1, dropping FIFOEN, so the FIFO setup from
	 * MX_USART3_UART_Init is repeated afterwards. */
	huart3.Init.BaudRate = BENCH_BAUD;
	huart3.Init.OverSampling = (BENCH_BAUD > 9000000u) ? UART_OVERSAMPLING_8
	                                                   : UART_OVERSAMPLING_16;
	if (HAL_UART_Init(&huart3) != HAL_OK ||
	    HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK ||
	    HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK ||
	    HAL_UARTEx_EnableFifoMode(&huart3) != HAL_OK) {
		Error_Handler();
	}

	std::memset(s_tx_frame, 0x55, sizeof s_tx_frame);

	s_uart.setRxHandler([](std::span<const uint8_t> bytes) { onRx(bytes); });
	s_uart.setRxGapHandler([]() { s_win.gaps += 1u; });

	if (!s_uart.init(&huart3)) {
		Error_Handler(); // misconfiguration: the bench must not pretend to run
	}
	resetWindow();
}

extern "C" void bench_loop(void)
{
	s_uart.proceed();

	if (s_pending_baud != 0u && !s_uart.tx_busy()) {
		const uint32_t baud = s_pending_baud;
		s_pending_baud = 0u;
		s_uart.setBaudRate(baud); // failure leaves the old speed running
		return;
	}

	if (s_report_pending) {
		if (!s_uart.tx_busy()) {
			buildReport();
			if (s_uart.send(std::span<const uint8_t>{
					reinterpret_cast<const uint8_t*>(s_report),
					std::strlen(s_report)})) {
				s_report_pending = false;
			}
		}
		return; // the report outranks the generator
	}
	if (s_tx_gen && !s_uart.tx_busy()) {
		if (s_uart.send(std::span<const uint8_t>{s_tx_frame, sizeof s_tx_frame})) {
			s_win.tx_frames += 1u;
		}
	}
}
