/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * uart_probe.h — benchmark instrumentation for uart/Uart.h. NOT part of the
 * UART API: in a normal build (UART_ENGINE_PROBE=0, the default) this file
 * compiles to absolutely nothing — no fields in Uart, no RAM, no instructions
 * (proven by the disassembly-identity build in uart/tests/port/build.sh).
 *
 * With UART_ENGINE_PROBE=1 three sites are measured in DWT cycles:
 *   Rx      — isrRxEvent(), the whole RX hot path of THIS driver;
 *   Slow    — proceedSlow(), the periodic audit;
 *   TxStart — send(), the TX start path.
 * That is deliberately only the library's own cost. The HAL machinery runs
 * BEFORE our callbacks, so the full IRQ cost must be measured in the
 * application, around HAL_UART_IRQHandler / HAL_DMA_IRQHandler:
 *
 *     void USART3_IRQHandler(void) {
 *         const uint32_t start = DWT->CYCCNT;
 *         HAL_UART_IRQHandler(&huart3);
 *         g_usart_irq.add(DWT->CYCCNT - start);   // a uart_probe::Counter
 *     }
 *
 * full IRQ − our Rx counter ≈ ST/HAL overhead per event.
 *
 * Usage: enable the define, call uart_probe::init() once from the application
 * (the library never touches CoreDebug), read uart_probe::snapshot() from the
 * main loop. Counters are written by one context each and read via snapshot()
 * under PRIMASK — no atomics: instrumentation must not measure itself.
 */

#ifndef UART_ENGINE_UART_PROBE_H_
#define UART_ENGINE_UART_PROBE_H_

#include <cstdint>

#ifndef UART_ENGINE_PROBE
#define UART_ENGINE_PROBE 0
#endif

namespace uart_probe {

enum class Site : uint8_t { Rx, Slow, TxStart };

#if UART_ENGINE_PROBE

// Capability, not family: whatever core CMSIS exposes CYCCNT on will do.
#if !defined(DWT) || !defined(DWT_CTRL_CYCCNTENA_Msk)
#	error "[UART PROBE]: UART_ENGINE_PROBE=1 needs DWT CYCCNT (absent on Cortex-M0/M0+); include the device CMSIS before uart_probe.h"
#endif

struct Counter {
	uint64_t total = 0u;  // CYCCNT subtraction is wrap-safe; total is 64-bit
	uint32_t max   = 0u;
	uint32_t calls = 0u;

	void add(const uint32_t elapsed) noexcept
	{
		total += elapsed;
		if (elapsed > max) { max = elapsed; }
		++calls;
	}
};

struct Stats {
	Counter rx;
	Counter slow;
	Counter tx_start;
};

inline Stats g_stats{};

// One-time, called by the APPLICATION before measuring.
inline void init() noexcept
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#if defined(__CORTEX_M) && (__CORTEX_M == 7U)
	// CM7 gates DWT behind the CoreSight software lock; CMSIS has no named
	// field for it, so write the key (ARM DDI 0489) to DWT_BASE + 0xFB0.
	*reinterpret_cast<volatile uint32_t*>(DWT_BASE + 0xFB0u) = 0xC5ACCE55u;
#endif
	DWT->CYCCNT = 0u;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

// Coherent copy for the main loop; each counter group has a single writer,
// so a short PRIMASK window is all the synchronization there is to need.
inline Stats snapshot() noexcept
{
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	const Stats copy = g_stats;
	__set_PRIMASK(primask);
	return copy;
}

class Scope final {
public:
	explicit Scope(const Site site) noexcept
		: m_counter(counterFor(site)), m_start(DWT->CYCCNT) {}
	~Scope() { m_counter.add(DWT->CYCCNT - m_start); } // wraparound-safe

	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;

private:
	static Counter& counterFor(const Site site) noexcept
	{
		switch (site) {
		case Site::Rx:   return g_stats.rx;
		case Site::Slow: return g_stats.slow;
		default:         return g_stats.tx_start;
		}
	}

	Counter& m_counter;
	const uint32_t m_start;
};

#else /* UART_ENGINE_PROBE == 0: the probe does not exist */

class Scope final {
public:
	constexpr explicit Scope(Site) noexcept {}
	Scope(const Scope&) = delete;
	Scope& operator=(const Scope&) = delete;
};

#endif /* UART_ENGINE_PROBE */

} // namespace uart_probe

// #ifndef so the port matrix can stub the macro out from the command line and
// prove by objdump that the disabled Scope really generates zero code.
#ifndef UART_ENGINE_PROBE_SCOPE
#define UART_ENGINE_PROBE_CAT_(a, b) a##b
#define UART_ENGINE_PROBE_CAT(a, b)  UART_ENGINE_PROBE_CAT_(a, b)
#define UART_ENGINE_PROBE_SCOPE(site) \
	[[maybe_unused]] const ::uart_probe::Scope \
	UART_ENGINE_PROBE_CAT(uart_engine_probe_scope_, __LINE__){::uart_probe::Site::site}
#endif

#endif /* UART_ENGINE_UART_PROBE_H_ */
