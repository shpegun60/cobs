/*
 * Portability smoke test: compiles the whole UART engine for a concrete
 * STM32 target, instantiating every template path and every API entry.
 * Compile-only (-c): HAL functions are declared by the family headers but
 * never linked, so NOTHING here verifies runtime behaviour — it proves the
 * code compiles and that every path is type-checked against a real HAL.
 * Built by build.sh for each target in this directory.
 */

#define UART_ENGINE_IMPLEMENT // emit the HAL callback definitions here
#include "Uart.h"

static UART_HandleTypeDef s_huart;
static DMA_HandleTypeDef  s_dma_rx;
static DMA_HandleTypeDef  s_dma_tx;

static UART_HandleTypeDef s_huart2;
static DMA_HandleTypeDef  s_dma_rx2;
static DMA_HandleTypeDef  s_dma_tx2;

static Uart<256, 4> s_uart;
// A second, differently parameterized instance: proves the template is not
// accidentally valid only for one <ChunkSize, ChunkCount> pair, and puts a
// second entry in the static registry.
static Uart<64, 2> s_uart2;

extern "C" int uart_port_test()
{
	s_huart.hdmarx = &s_dma_rx;
	s_huart.hdmatx = &s_dma_tx;
	s_huart2.hdmarx = &s_dma_rx2;
	s_huart2.hdmatx = &s_dma_tx2;

	bool ok = s_uart.init(&s_huart);
	ok = ok && s_uart2.init(&s_huart2);

	// One-shot binding contract (behaviour NOT verified here — needs a
	// runtime test): a second engine must refuse a handle that is already
	// owned, and must stay unbound so its destructor never aborts a
	// peripheral belonging to another instance:
	//     Uart<> a, b;
	//     a.init(&h) == true; b.init(&h) == false; b.instance() == nullptr;
	// Re-binding an already-bound object must also be refused.
	const bool duplicate_refused = !s_uart2.init(&s_huart);
	const bool rebind_refused    = !s_uart.init(&s_huart2);

	s_uart.setRxHandler([](std::span<const uint8_t> bytes) { (void)bytes; });
	s_uart.setTxHandler([](bool tx_ok) { (void)tx_ok; });
	s_uart.setErrorHandler([](uint32_t code) { (void)code; });
	s_uart.setRxGapHandler([]() { /* decoder would drop its partial frame */ });

	s_uart.proceed(0u);
	s_uart.proceed();   // default argument (HAL_GetTick) instantiation
	s_uart2.proceed(0u);

	static const uint8_t frame[8] = {1, 2, 3, 4, 5, 6, 7, 0};
	ok = ok && s_uart.send(std::span<const uint8_t>{frame, sizeof frame});

	(void)s_uart.tx_busy();
	(void)s_uart.stats();
	(void)s_uart.instance();

#if UART_ENGINE_PROBE
	// Probe-on builds: force codegen of the DWT backend, not just parsing.
	uart_probe::init();
	const uart_probe::Stats probe_stats = uart_probe::snapshot();
	(void)probe_stats;
#endif
	return (ok && duplicate_refused && rebind_refused) ? 0 : 1;
}
