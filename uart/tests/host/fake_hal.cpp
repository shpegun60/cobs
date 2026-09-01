/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Behaviour of the fake HAL. It deliberately models the REAL HAL quirks we
 * verified in the F1/G4/H7RS sources, not a convenient version of them:
 *   - IDLE and TC both END the reception and set RxState READY BEFORE the
 *     RxEvent callback runs;
 *   - with DMA reception active every RX error is blocking: the transfer is
 *     ended and the DMA aborted before HAL_UART_ErrorCallback;
 *   - an abort may raise the completion callback of the transfer it
 *     interrupts (ST documents this in HAL_UART_DMAStop);
 *   - aborts can return HAL_TIMEOUT, leaving the transfer live.
 */
#include "fake_hal.h"
#include <cstring>
#include <functional>
#include <vector>

namespace fake {

uint32_t g_primask = 0u;

void dispatch_pending() noexcept; // declared in cmsis_compiler.h, defined below

namespace {
Model g_model;
std::vector<std::function<void()>> g_pending;
bool g_dispatching = false;
} // namespace

Model& model() noexcept { return g_model; }

void fail(const std::string& what) noexcept { g_model.violations.push_back(what); }

void reset() noexcept
{
	g_model = Model{};
	g_pending.clear();
	g_primask = 0u;
	g_dispatching = false;
}

// Raise an interrupt: it runs now only if PRIMASK allows it, otherwise it
// becomes pending. That is the whole point of the harness.
static void raise(std::function<void()> fn) noexcept
{
	g_pending.push_back(std::move(fn));
	dispatch_pending();
}

static void notifyRx(UART_HandleTypeDef* const h, const uint16_t size) noexcept
{
#if USE_HAL_UART_REGISTER_CALLBACKS == 1
	if (h->RxEventCallback != nullptr) { h->RxEventCallback(h, size); }
#else
	HAL_UARTEx_RxEventCallback(h, size);
#endif
}

static void notifyTx(UART_HandleTypeDef* const h) noexcept
{
#if USE_HAL_UART_REGISTER_CALLBACKS == 1
	if (h->TxCpltCallback != nullptr) { h->TxCpltCallback(h); }
#else
	HAL_UART_TxCpltCallback(h);
#endif
}

static void notifyError(UART_HandleTypeDef* const h) noexcept
{
#if USE_HAL_UART_REGISTER_CALLBACKS == 1
	if (h->ErrorCallback != nullptr) { h->ErrorCallback(h); }
#else
	HAL_UART_ErrorCallback(h);
#endif
}

void dispatch_pending() noexcept
{
	if (g_primask != 0u || g_dispatching) {
		return;
	}
	g_dispatching = true;
	while (!g_pending.empty() && g_primask == 0u) {
		auto fn = g_pending.front();
		g_pending.erase(g_pending.begin());
		fn();
	}
	g_dispatching = false;
}

/* ------------------------- ownership bookkeeping ------------------------ */

static void mark(const void* buf, Slot s) noexcept
{
	if (buf != nullptr) {
		g_model.slots[buf] = s;
	}
}

void note_consumer_sees(const void* buf) noexcept
{
	if (g_model.rx_armed && buf == g_model.rx_dst) {
		fail("consumer was handed the buffer the DMA currently owns");
	}
	const auto it = g_model.slots.find(buf);
	if (it != g_model.slots.end() && it->second == Slot::DmaOwned) {
		fail("consumer sees a DMA-owned slot");
	}
	mark(buf, Slot::ConsumerVisible);
}

void note_consumer_done(const void* buf) noexcept { mark(buf, Slot::Free); }

void clear_armed_history() noexcept { g_model.armed_history.clear(); }

std::size_t distinct_chunks_armed() noexcept
{
	std::vector<const void*> uniq;
	for (const void* p : g_model.armed_history) {
		bool seen = false;
		for (const void* q : uniq) {
			if (p == q) { seen = true; break; }
		}
		if (!seen) { uniq.push_back(p); }
	}
	return uniq.size();
}

/* ----------------------------- bus events ------------------------------- */

void advance_tick(uint32_t ms) noexcept { g_model.tick += ms; }

void rx_bytes(const void* data, std::size_t n) noexcept
{
	if (!g_model.rx_armed) {
		fail("bytes arrived while no RX transfer was armed");
		return;
	}
	if (n > g_model.huart->hdmarx->CountRemaining) {
		fail("DMA asked to write past the end of its buffer");
		return;
	}
	const std::size_t written = g_model.rx_len - g_model.huart->hdmarx->CountRemaining;
	std::memcpy(g_model.rx_dst + written, data, n);
	g_model.huart->hdmarx->CountRemaining -= static_cast<uint32_t>(n);
}

// Common tail of IDLE/TC: the HAL ends the reception first, then calls back.
static void endRxAndNotify(HAL_UART_RxEventTypeTypeDef evt) noexcept
{
	UART_HandleTypeDef* const h = g_model.huart;
	const uint16_t got = static_cast<uint16_t>(g_model.rx_len - h->hdmarx->CountRemaining);

	h->Instance->CR3 &= ~USART_CR3_DMAR;
	h->RxState = HAL_UART_STATE_READY;
	h->RxEventType = evt;
	h->hdmarx->State = HAL_DMA_STATE_READY;
	mark(g_model.rx_dst, Slot::Free); // no longer DMA-owned; the driver still holds the claim
	g_model.rx_armed = false;

	raise([got]() { notifyRx(g_model.huart, got); });
}

void rx_idle() noexcept
{
	if (!g_model.rx_armed) { return; }
	endRxAndNotify(HAL_UART_RXEVENT_IDLE);
}

void rx_tc() noexcept
{
	if (!g_model.rx_armed) { return; }
	g_model.huart->hdmarx->CountRemaining = 0u;
	endRxAndNotify(HAL_UART_RXEVENT_TC);
}

void rx_half() noexcept
{
	if (!g_model.rx_armed) { return; }
	g_model.huart->RxEventType = HAL_UART_RXEVENT_HT;
	const uint16_t got = static_cast<uint16_t>(
		g_model.rx_len - g_model.huart->hdmarx->CountRemaining);
	raise([got]() { notifyRx(g_model.huart, got); });
}

void rx_corrupt_counter(const uint32_t remaining) noexcept
{
	if (!g_model.rx_armed) { return; }
	auto* const h = g_model.huart;
	h->Instance->CR3 &= ~USART_CR3_DMAR;
	h->RxState = HAL_UART_STATE_READY;
	h->RxEventType = HAL_UART_RXEVENT_IDLE;
	h->hdmarx->CountRemaining = remaining;
	h->hdmarx->State = HAL_DMA_STATE_READY;
	mark(g_model.rx_dst, Slot::Free);
	g_model.rx_armed = false;
	raise([]() { notifyRx(g_model.huart, 0u); });
}

void rx_error(uint32_t code) noexcept
{
	UART_HandleTypeDef* const h = g_model.huart;
	h->ErrorCode |= code;
	// "any error occurs in DMA mode reception" is blocking: end RX, abort DMA.
	if (g_model.rx_armed) {
		h->Instance->CR3 &= ~USART_CR3_DMAR;
		h->RxState = HAL_UART_STATE_READY;
		h->hdmarx->State = HAL_DMA_STATE_READY;
		mark(g_model.rx_dst, Slot::Free);
		g_model.rx_armed = false;
	}
	raise([]() { notifyError(g_model.huart); });
}

// Stage 1: the DMA has moved the last byte into the peripheral. The HAL only
// clears DMAT and enables TCIE here — TxCpltCallback comes later, from TC.
void tx_dma_done() noexcept
{
	if (!g_model.tx_armed) { return; }
	g_model.huart->hdmatx->CountRemaining = 0u;
	g_model.huart->Instance->CR3 &= ~USART_CR3_DMAT;
	g_model.huart->hdmatx->State = HAL_DMA_STATE_READY;
}

// Stage 2: the shift register is empty, TC is set, the completion is raised.
void tx_uart_tc() noexcept
{
	g_model.huart->Instance->ISR |= USART_ISR_TC;
	if (!g_model.tx_armed) { return; }
	g_model.tx_armed = false;
	g_model.huart->gState = HAL_UART_STATE_READY;
	raise([]() { notifyTx(g_model.huart); });
}

void tx_done() noexcept
{
	tx_dma_done();
	tx_uart_tc();
}

// The DMA moved some bytes without finishing: what a healthy long transfer
// looks like to the progress watchdog.
void tx_progress(uint16_t moved) noexcept
{
	auto* const h = g_model.huart;
	if (!g_model.tx_armed) { return; }
	h->hdmatx->CountRemaining = (h->hdmatx->CountRemaining > moved)
		? (h->hdmatx->CountRemaining - moved) : 0u;
}

void tx_error() noexcept
{
	g_model.huart->ErrorCode |= HAL_UART_ERROR_DMA;
	g_model.tx_armed = false;
	g_model.huart->gState = HAL_UART_STATE_READY;
	g_model.huart->Instance->CR3 &= ~USART_CR3_DMAT;
	raise([]() { notifyError(g_model.huart); });
}

} // namespace fake

/* ------------------------------- HAL API -------------------------------- */

extern "C" {

uint32_t HAL_GetTick(void) { return fake::model().tick; }

uint32_t HAL_UART_GetError(const UART_HandleTypeDef* h) { return h->ErrorCode; }
uint32_t HAL_DMA_GetError(const DMA_HandleTypeDef* h) { return h->ErrorCode; }

HAL_UART_RxEventTypeTypeDef HAL_UARTEx_GetRxEventType(const UART_HandleTypeDef* h)
{
	++fake::model().rx_event_type_calls;
	return h->RxEventType;
}

HAL_StatusTypeDef HAL_DMA_Init(DMA_HandleTypeDef* h)
{
	auto& m = fake::model();
	++m.dma_init_calls;
	if (m.fail_dma_init > 0) {
		--m.fail_dma_init;
		h->State = HAL_DMA_STATE_TIMEOUT;
		h->ErrorCode |= HAL_DMA_ERROR_TIMEOUT;
		return HAL_ERROR;
	}
	if (m.huart != nullptr && h == m.huart->hdmarx) {
		if (m.rx_armed) { fake::note_consumer_done(m.rx_dst); }
		m.rx_armed = false;
	}
	if (m.huart != nullptr && h == m.huart->hdmatx) {
		m.tx_armed = false;
	}
	h->State = HAL_DMA_STATE_READY;
	h->ErrorCode = HAL_DMA_ERROR_NONE;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_RegisterRxEventCallback(
	UART_HandleTypeDef* h, void (*callback)(UART_HandleTypeDef*, uint16_t))
{
	auto& m = fake::model();
	++m.callback_registration_calls;
	if (m.fail_callback_registration > 0 &&
			m.callback_registration_calls ==
				static_cast<uint32_t>(m.fail_callback_registration)) {
		return HAL_ERROR;
	}
	h->RxEventCallback = callback;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_RegisterCallback(
	UART_HandleTypeDef* h, const HAL_UART_CallbackIDTypeDef id,
	void (*callback)(UART_HandleTypeDef*))
{
	auto& m = fake::model();
	++m.callback_registration_calls;
	if (m.fail_callback_registration > 0 &&
			m.callback_registration_calls ==
				static_cast<uint32_t>(m.fail_callback_registration)) {
		return HAL_ERROR;
	}
	if (id == HAL_UART_TX_COMPLETE_CB_ID) {
		h->TxCpltCallback = callback;
		return HAL_OK;
	}
	if (id == HAL_UART_ERROR_CB_ID) {
		h->ErrorCallback = callback;
		return HAL_OK;
	}
	return HAL_ERROR;
}

// Models the real HAL_UART_Init faithfully in the two ways that matter here:
// it CLEARS FIFOEN and both threshold fields (they are inside the CR1/CR3
// masks it rewrites but never in the value written back), and on an
// unreachable baud rate it returns HAL_ERROR leaving gState BUSY.
HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef* h)
{
	auto& m = fake::model();
	++m.init_calls;
	if (h->Instance == nullptr) { return HAL_ERROR; }
	if (m.rx_armed || m.tx_armed) {
		fake::fail("the peripheral was reconfigured with a transfer still live");
	}

	h->gState = HAL_UART_STATE_BUSY;
	h->Instance->CR1 &= ~USART_CR1_FIFOEN;
	h->Instance->CR3 &= ~(USART_CR3_TXFTCFG | USART_CR3_RXFTCFG);

	// A rate the kernel clock cannot produce: the real UART_SetConfig bails
	// out here, leaving the handle BUSY and the peripheral disabled.
	if (h->Init.BaudRate > m.max_baud) { return HAL_ERROR; }

	m.applied_baud = h->Init.BaudRate;
	h->ErrorCode = HAL_UART_ERROR_NONE;
	h->gState = HAL_UART_STATE_READY;
	h->RxState = HAL_UART_STATE_READY;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UARTEx_EnableFifoMode(UART_HandleTypeDef* h)
{
	if (fake::model().fail_fifo_config > 0) {
		--fake::model().fail_fifo_config;
		return HAL_ERROR;
	}
	h->FifoMode = UART_FIFOMODE_ENABLE;
	h->Instance->CR1 |= USART_CR1_FIFOEN;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UARTEx_SetTxFifoThreshold(UART_HandleTypeDef* h, uint32_t thr)
{
	if (fake::model().fail_fifo_config > 0) {
		--fake::model().fail_fifo_config;
		return HAL_ERROR;
	}
	h->Instance->CR3 = (h->Instance->CR3 & ~USART_CR3_TXFTCFG) | (thr & USART_CR3_TXFTCFG);
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UARTEx_SetRxFifoThreshold(UART_HandleTypeDef* h, uint32_t thr)
{
	if (fake::model().fail_fifo_config > 0) {
		--fake::model().fail_fifo_config;
		return HAL_ERROR;
	}
	h->Instance->CR3 = (h->Instance->CR3 & ~USART_CR3_RXFTCFG) | (thr & USART_CR3_RXFTCFG);
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef* h, uint8_t* dst, uint16_t len)
{
	auto& m = fake::model();
	if (m.fail_arm > 0) { --m.fail_arm; return HAL_ERROR; }
	if (m.rx_armed) {
		fake::fail("RX armed twice without an intervening stop");
		return HAL_BUSY;
	}
	const auto it = m.slots.find(dst);
	if (it != m.slots.end() && it->second == fake::Slot::ConsumerVisible) {
		fake::fail("DMA armed onto a buffer the consumer is reading");
	}

	m.rx_armed = true;
	m.rx_dst = dst;
	m.rx_len = len;
	m.armed_history.push_back(dst);
	m.slots[dst] = fake::Slot::DmaOwned;

	h->hdmarx->CountRemaining = len;
	h->hdmarx->State = HAL_DMA_STATE_BUSY;
	// All audited STM32 HALs enable HT on every DMA start. The driver must
	// explicitly clear it again after each successful arm.
	h->hdmarx->Instance->dummy |= DMA_IT_HT;
	h->RxState = HAL_UART_STATE_BUSY_RX;
	h->RxXferSize = len;
	h->Instance->CR3 |= USART_CR3_DMAR;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef* h, const uint8_t* src, uint16_t len)
{
	auto& m = fake::model();
	if (m.tx_armed) {
		fake::fail("TX started while a transfer was still active");
		return HAL_BUSY;
	}
	m.tx_armed = true;
	m.tx_src = src;
	m.tx_len = len;
	h->hdmatx->CountRemaining = len;
	h->hdmatx->Instance->dummy |= DMA_IT_HT;
	h->Instance->ISR &= ~USART_ISR_TC; // TC clears when a new transfer starts
	h->gState = HAL_UART_STATE_BUSY_TX;
	h->hdmatx->State = HAL_DMA_STATE_BUSY;
	h->Instance->CR3 |= USART_CR3_DMAT;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef* h)
{
	auto& m = fake::model();
	if (m.rx_cplt_inside_abort && m.rx_armed) {
		m.rx_cplt_inside_abort = false;
		fake::rx_idle(); // ST: the interrupted transfer may still raise its callback
	}
	if (m.tx_error_inside_abort_receive) {
		m.tx_error_inside_abort_receive = false;
		fake::tx_error();
	}
	// Real HAL disables the UART request before HAL_DMA_Abort. If that abort
	// times out, a retry through HAL_UART_AbortReceive alone skips the DMA
	// because DMAR is already clear; the driver must repair the DMA handle.
	h->Instance->CR3 &= ~USART_CR3_DMAR;
	if (m.fail_abort_receive > 0) {
		--m.fail_abort_receive;
		h->ErrorCode |= HAL_UART_ERROR_DMA;
		h->hdmarx->ErrorCode |= HAL_DMA_ERROR_TIMEOUT;
		h->hdmarx->State = HAL_DMA_STATE_TIMEOUT;
		return HAL_TIMEOUT; // transfer deliberately left live
	}
	if (m.rx_armed) {
		fake::note_consumer_done(m.rx_dst);
		m.rx_armed = false;
	}
	h->hdmarx->State = HAL_DMA_STATE_READY;
	h->RxState = HAL_UART_STATE_READY;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef* h)
{
	auto& m = fake::model();
	if (m.tx_cplt_inside_abort && m.tx_armed) {
		m.tx_cplt_inside_abort = false;
		fake::tx_done();
	}
	if (m.rx_error_inside_abort_transmit) {
		m.rx_error_inside_abort_transmit = false;
		fake::rx_error(HAL_UART_ERROR_ORE);
	}
	h->Instance->CR3 &= ~USART_CR3_DMAT;
	if (m.fail_abort_transmit > 0) {
		--m.fail_abort_transmit;
		h->ErrorCode |= HAL_UART_ERROR_DMA;
		h->hdmatx->ErrorCode |= HAL_DMA_ERROR_TIMEOUT;
		h->hdmatx->State = HAL_DMA_STATE_TIMEOUT;
		return HAL_TIMEOUT;
	}
	m.tx_armed = false;
	h->hdmatx->State = HAL_DMA_STATE_READY;
	h->gState = HAL_UART_STATE_READY;
	return HAL_OK;
}

HAL_StatusTypeDef HAL_UART_Abort(UART_HandleTypeDef* h)
{
	// Same order and early-return behaviour as the audited F1/G4/H7RS HALs.
	if (HAL_UART_AbortTransmit(h) != HAL_OK) { return HAL_TIMEOUT; }
	return HAL_UART_AbortReceive(h);
}

HAL_StatusTypeDef HAL_UART_DMAStop(UART_HandleTypeDef* h)
{
	(void)h;
	return HAL_OK;
}

} // extern "C"
