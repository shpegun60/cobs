/*
 * Uart.h — zero-copy STM32 UART byte transport (always-DMA, buffer switching)
 *
 * Design (see doc/UART_COBS_ARCHITECTURE.md, §3–§8):
 *  - One mode only: DMA normal (non-circular) via HAL_UARTEx_ReceiveToIdle_DMA.
 *  - RX: spsc::cache_aligned_chunk_fifo — DMA writes directly into a claimed
 *    chunk slot; the ISR only commits the size, publishes the chunk and
 *    re-arms DMA onto the next free slot. Zero memcpy, zero heap.
 *  - TX: no queue. send(span) starts DMA on caller-owned memory, tx_busy()
 *    reports completion. Policy on Busy belongs to the layer above (COBS).
 *  - Portable across STM32 series: register differences live in uart_regs.h,
 *    D-cache maintenance is compile-time optional, HAL callbacks attach either
 *    internally or via USE_HAL_UART_REGISTER_CALLBACKS.
 *
 * Memory placement is the USER's responsibility: chunk storage is inline in
 * this object, so the whole instance must live in DMA-accessible RAM, e.g.
 *
 *   __attribute__((section(".dma")))
 *   static Uart<256, 4> uart3;
 *
 * (On H7: not in DTCM. If the section is made non-cacheable via MPU, set
 *  UART_ENGINE_DCACHE_MAINTENANCE to 0.)
 *
 * Interrupt priorities: keep the UART global IRQ and the RX DMA IRQ at the
 * SAME preemption priority (the CubeMX default). If one may preempt the
 * other, a rare benign race exists where the UART error ISR interrupts the
 * DMA TC ISR between HAL's RxState update and this driver's re-arm and voids
 * one valid chunk — no corruption, the stream resynchronizes, but the frame
 * is lost.
 */

#ifndef UART_ENGINE_UART_H_
#define UART_ENGINE_UART_H_

#include "main.h" // must provide the HAL / UART_HandleTypeDef for the target series

// This driver targets the asynchronous UART HAL exclusively: every symbol it
// uses (UART_HandleTypeDef, HAL_UARTEx_ReceiveToIdle_DMA, ...) is declared
// only under HAL_UART_MODULE_ENABLED. Accepting HAL_USART_MODULE_ENABLED as
// well would let a synchronous-USART-only project through this guard and fail
// deep inside the body instead of compiling out cleanly.
#if defined(HAL_UART_MODULE_ENABLED)

#include "uart_regs.h"       // SR/DR vs ISR/RDR abstraction (all STM32 series)
#include "irq/IRQGuard.h"

#include "spsc/chunk_fifo.hpp"   // https://github.com/shpegun60/spsc
#include "tiny_delegate.hpp"     // https://github.com/shpegun60/delegate

#include <array>
#include <cstdint>

#if !defined(__cplusplus) || (__cplusplus < 202002L)
#	error "[UART ENGINE]: C++20 is required (std::span) — build with -std=gnu++20 or newer"
#endif
#include <span>

/* ============================== configuration ============================== */

// Maximum number of Uart instances (static registry, no heap).
#ifndef UART_ENGINE_MAX_INSTANCES
#	define UART_ENGINE_MAX_INSTANCES 4
#endif

// D-cache maintenance around DMA buffers (invalidate RX chunk before publish,
// clean TX span before send). Auto-enabled when the core has a D-cache
// (F7/H7...). Set to 0 explicitly when the DMA memory region is configured
// non-cacheable through the MPU — then maintenance is unnecessary.
#ifndef UART_ENGINE_DCACHE_MAINTENANCE
#	if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
#		define UART_ENGINE_DCACHE_MAINTENANCE 1
#	else
#		define UART_ENGINE_DCACHE_MAINTENANCE 0
#	endif
#endif

// 1 -> this library defines HAL_UARTEx_RxEventCallback / HAL_UART_TxCpltCallback /
//      HAL_UART_ErrorCallback (define UART_ENGINE_IMPLEMENT in exactly ONE .cpp).
// 0 -> the user delegates their own callbacks to UartRegistry::onRxEvent/onTxCplt/onError.
// With USE_HAL_UART_REGISTER_CALLBACKS == 1 neither is needed: init() registers
// per-handle callbacks and nothing global is defined.
#ifndef UART_ENGINE_INTERNAL_CALLBACKS_ON
#	define UART_ENGINE_INTERNAL_CALLBACKS_ON 1
#endif

// Health-check watchdog period inside proceed(). Every period the HAL/DMA
// state is inspected; UART_ENGINE_FAIL_THRESHOLD consecutive bad checks
// trigger a full receiver/transmitter recovery.
#ifndef UART_ENGINE_CHECK_PERIOD_MS
#	define UART_ENGINE_CHECK_PERIOD_MS 200
#endif
#ifndef UART_ENGINE_FAIL_THRESHOLD
#	define UART_ENGINE_FAIL_THRESHOLD 3
#endif

// Extra margin added to the computed TX deadline (deadline is derived from
// the actual baud rate and frame length, so it adapts to any speed).
#ifndef UART_ENGINE_TX_TIMEOUT_MARGIN_MS
#	define UART_ENGINE_TX_TIMEOUT_MARGIN_MS 50
#endif

// The IDLE/HT/TC events themselves are HARDWARE features present on every
// STM32 (IDLE flag on all USART IPs, HT/TC on all DMA/BDMA/GPDMA types).
// But HAL_UARTEx_GetRxEventType() and the HAL_UART_RXEVENT_* enum are a HAL
// add-on introduced LATER than ReceiveToIdle itself, per-family; all current
// packages have them, an older cached HAL may not. They are enum values (not
// #defines), so presence cannot be auto-detected by the preprocessor — set
// this to 0 if the compiler reports HAL_UARTEx_GetRxEventType undeclared:
// the event kind is then derived from RxState (READY -> TC, still
// receiving -> IDLE/HT), which is equivalent for this driver.
#ifndef UART_ENGINE_HAS_RXEVENT_TYPE
#	define UART_ENGINE_HAS_RXEVENT_TYPE 1
#endif

/* ============================ D-cache helpers ============================= */

namespace uart_detail {

#if UART_ENGINE_DCACHE_MAINTENANCE

static inline bool dcacheEnabled() noexcept
{
	return (SCB->CCR & SCB_CCR_DC_Msk) != 0u;
}

static inline void dcacheInvalidate(const void* const addr, const uint32_t size) noexcept
{
	if (addr && size && dcacheEnabled()) {
		SCB_InvalidateDCache_by_Addr(const_cast<void*>(addr), static_cast<int32_t>(size));
	}
}

static inline void dcacheClean(const void* const addr, const uint32_t size) noexcept
{
	if (addr && size && dcacheEnabled()) {
		SCB_CleanDCache_by_Addr(const_cast<void*>(addr), static_cast<int32_t>(size));
	}
}

#else /* stubs — no D-cache on this core, or MPU region is non-cacheable */

static inline void dcacheInvalidate(const void*, const uint32_t) noexcept {}
static inline void dcacheClean(const void*, const uint32_t) noexcept {}

#endif /* UART_ENGINE_DCACHE_MAINTENANCE */

} // namespace uart_detail

/* ============================ instance registry =========================== */

// Non-template, non-virtual dispatch from HAL callbacks to instances.
// Fixed-capacity static table: no heap, no std::vector, no vtables.
// The linear search runs over <= UART_ENGINE_MAX_INSTANCES entries per event.
class UartRegistry final {
public:
	struct Ops {
		void (*rx_event)(void* self, uint16_t size);
		void (*tx_cplt)(void* self);
		void (*error)(void* self);
	};

	static bool attach(UART_HandleTypeDef* const huart, void* const self, const Ops& ops) noexcept
	{
		if (!huart || !self) {
			return false;
		}
		// Guarded: other instances may already be running and their ISRs walk
		// this table; an Entry assignment is not atomic.
		IRQGuard guard;
		for (const auto& e : s_entries) {
			if (e.huart == huart) {
				return false; // one engine per UART: a duplicate would steal events
			}
		}
		for (auto& e : s_entries) {
			if (e.huart == nullptr) {
				e = Entry{huart, self, ops};
				return true;
			}
		}
		return false; // table full: raise UART_ENGINE_MAX_INSTANCES
	}

	static void detach(void* const self) noexcept
	{
		IRQGuard guard;
		for (auto& e : s_entries) {
			if (e.self == self) {
				e = Entry{};
			}
		}
	}

	/* Entry points, called from HAL callbacks (ISR context). */

	static void onRxEvent(UART_HandleTypeDef* const huart, const uint16_t size) noexcept
	{
		if (Entry* const e = find(huart)) {
			e->ops.rx_event(e->self, size);
		}
	}

	static void onTxCplt(UART_HandleTypeDef* const huart) noexcept
	{
		if (Entry* const e = find(huart)) {
			e->ops.tx_cplt(e->self);
		}
	}

	static void onError(UART_HandleTypeDef* const huart) noexcept
	{
		if (Entry* const e = find(huart)) {
			e->ops.error(e->self);
		}
	}

private:
	// Plain aggregate (no default member initializers): the static inline
	// s_entries{} below value-initializes it, which zeroes every field; NSDMIs
	// here would additionally require Entry to be complete before the end of
	// UartRegistry, which C++ forbids for the in-class definition.
	struct Entry {
		UART_HandleTypeDef* huart;
		void* self;
		Ops ops;
	};

	static Entry* find(const UART_HandleTypeDef* const huart) noexcept
	{
		for (auto& e : s_entries) {
			if (e.huart == huart) {
				return &e;
			}
		}
		return nullptr;
	}

	static inline std::array<Entry, UART_ENGINE_MAX_INSTANCES> s_entries{};
};

/* ================================ engine ================================== */

template<std::size_t ChunkSize = 256, std::size_t ChunkCount = 4>
class Uart final {
	static_assert(ChunkSize >= 32, "ChunkSize must be >= 32");
	static_assert(ChunkSize <= 65535, "HAL reception length is u16");
	static_assert((ChunkSize % 4) == 0, "ChunkSize must be word-aligned for DMA");
	static_assert(ChunkCount >= 2, "need at least 2 chunks to switch buffers");
	static_assert((ChunkCount & (ChunkCount - 1)) == 0, "ChunkCount must be a power of two (spsc)");
	// With cache maintenance active (M7 cores), invalidate/clean operate on
	// whole 32-byte cache lines: the chunk payload must fill whole lines and
	// be 32-byte aligned, otherwise neighbouring data (chunk metadata, the
	// next chunk) is destroyed by the invalidate. Preferred M7 configuration:
	// put the instance in an MPU non-cacheable region and set
	// UART_ENGINE_DCACHE_MAINTENANCE to 0.
	static_assert(!UART_ENGINE_DCACHE_MAINTENANCE || (ChunkSize % 32) == 0,
		"D-cache maintenance requires ChunkSize to be a multiple of the 32-byte cache line");

public:
	// cache-aligned metadata + single-writer atomic backend: ISR producer, loop consumer
	using RxFifo  = spsc::cache_aligned_chunk_fifo<uint8_t, ChunkSize, ChunkCount>;
	using RxChunk = typename RxFifo::value_type; // cache_aligned_slot IS-A spsc::chunk

	// Cache-safety proof for M7 maintenance mode: chunk payload (storage_) is
	// the FIRST member, so data() sits at the slot start; with the slot
	// aligned to >= one cache line and ChunkSize a multiple of 32 the payload
	// occupies whole cache lines — invalidating it can never touch the len_
	// field or a neighbouring slot.
	static_assert(!UART_ENGINE_DCACHE_MAINTENANCE || (alignof(RxChunk) % 32) == 0,
		"D-cache maintenance requires cache-line-aligned chunk slots");

	using RxHandler    = tiny::delegate<void(std::span<const uint8_t>)>;
	using TxHandler    = tiny::delegate<void(bool ok)>;
	using ErrorHandler = tiny::delegate<void(uint32_t halErrorCode)>;

	struct Stats {
		uint32_t rx_overrun   = 0; // no free chunk -> bytes discarded into drop buffer
		uint32_t rx_errors    = 0; // ORE/FE/NE/PE/DMA errors on the RX path
		uint32_t tx_errors    = 0;
		uint32_t restarts     = 0; // receiver restarts (error recovery)
	};

	Uart() = default;
	Uart(const Uart&) = delete;
	Uart& operator=(const Uart&) = delete;

	~Uart()
	{
		if (m_huart) {
			// Detach FIRST: from here on a HAL callback finds no registry
			// entry and cannot dispatch into an object that is being
			// destroyed — including while the abort below runs, or if that
			// abort times out. The abort itself is outside an IRQ guard
			// (HAL_DMA_Abort polls HAL_GetTick, frozen while masked).
			//
			// Residual hazard a destructor cannot fix: if the abort fails,
			// DMA keeps writing into storage that is about to go away. Give
			// instances static lifetime (the documented usage) and this
			// cannot arise.
			UartRegistry::detach(this);
			(void)HAL_UART_Abort(m_huart);
			m_huart = nullptr;
		}
	}

	/* ------------------------------ setup ------------------------------- */

	// Pre-flight validates EVERYTHING the runtime relies on; a false return
	// means the CubeMX/startup configuration is wrong, not a transient error.
	bool init(UART_HandleTypeDef* const huart) noexcept
	{
		/* --- structural checks ------------------------------------------ */
		if (!huart || !huart->Instance || !huart->hdmarx || !huart->hdmatx) {
			return false; // no handle, no peripheral, or DMA not linked (CubeMX)
		}
		if (huart->gState == HAL_UART_STATE_RESET) {
			return false; // HAL_UART_Init() was never run
		}
		if (huart->hdmarx->State == HAL_DMA_STATE_RESET ||
				huart->hdmatx->State == HAL_DMA_STATE_RESET) {
			return false; // HAL_DMA_Init() was never run for rx/tx
		}
		// The buffer-switching scheme REQUIRES a one-shot RX DMA: the
		// reception must END when the buffer fills or the line goes idle, so
		// that a published chunk is never written behind the consumer's back.
		// Both continuous modes must be rejected, and they are spelled
		// differently per DMA IP:
#ifdef DMA_CIRCULAR
		// Classic channel/stream DMA (F0/F1/F3/F4/F7/G0/G4/L4/H7 DMA1-2 ...).
		if (huart->hdmarx->Init.Mode == DMA_CIRCULAR) {
			return false;
		}
#endif
#ifdef DMA_LINKEDLIST
		// GPDMA/HPDMA (H5, H7RS, U5, WBA ...): DMA_CIRCULAR does not exist
		// there; the continuous variant is DMA_LINKEDLIST_CIRCULAR, which the
		// UART HAL itself tests before deciding whether to end reception on
		// IDLE. Plain linked-list mode is refused as well: HAL_DMA_Abort()
		// zeroes CBR1 for linked-list transfers, which would make the
		// received-count read in dmaReceivedCount() report a full chunk.
		if ((huart->hdmarx->Mode & DMA_LINKEDLIST) != 0u) {
			return false;
		}
#endif
		if (huart->Init.BaudRate == 0u) {
			return false; // TX deadline computation needs a real baud rate
		}

		/* --- registration ------------------------------------------------ */
		UartRegistry::detach(this); // re-init safe: drop a stale registration
		m_huart = huart;

		static constexpr UartRegistry::Ops ops = {
			[](void* self, uint16_t n) { static_cast<Uart*>(self)->isrRxEvent(n); },
			[](void* self)             { static_cast<Uart*>(self)->isrTxCplt(); },
			[](void* self)             { static_cast<Uart*>(self)->isrError(); },
		};
		if (!UartRegistry::attach(huart, this, ops)) {
			return false; // table full or another engine already owns this UART
		}

#if (USE_HAL_UART_REGISTER_CALLBACKS == 1)
		bool reg_ok = (HAL_UART_RegisterRxEventCallback(huart,
			[](UART_HandleTypeDef* h, uint16_t n) { UartRegistry::onRxEvent(h, n); }) == HAL_OK);
		reg_ok = reg_ok && (HAL_UART_RegisterCallback(huart, HAL_UART_TX_COMPLETE_CB_ID,
			[](UART_HandleTypeDef* h) { UartRegistry::onTxCplt(h); }) == HAL_OK);
		reg_ok = reg_ok && (HAL_UART_RegisterCallback(huart, HAL_UART_ERROR_CB_ID,
			[](UART_HandleTypeDef* h) { UartRegistry::onError(h); }) == HAL_OK);
		if (!reg_ok) {
			UartRegistry::detach(this);
			return false;
		}
#endif

		receiveRestart();
		return m_started;
	}

	// IRQ-guarded: a handler may be (re)assigned even while reception is
	// already running — the ISR never observes a half-written delegate.
	void setRxHandler(RxHandler h) noexcept
	{
		IRQGuard guard;
		m_rxHandler = static_cast<RxHandler&&>(h);
	}
	void setTxHandler(TxHandler h) noexcept
	{
		IRQGuard guard;
		m_txHandler = static_cast<TxHandler&&>(h);
	}
	void setErrorHandler(ErrorHandler h) noexcept
	{
		IRQGuard guard;
		m_errHandler = static_cast<ErrorHandler&&>(h);
	}

	/* ---------------------------- main loop ----------------------------- */

	// Drains published RX chunks and runs the self-healing watchdog. The span
	// handed to the handler is valid ONLY during the callback (the slot
	// returns to the DMA producer on pop). Call this from the main loop.
	void proceed(const uint32_t now_ms = HAL_GetTick()) noexcept
	{
		if (m_huart == nullptr) {
			return; // not initialized (or init() failed) — nothing to service
		}

		while (RxChunk* const c = m_rx.try_front()) {
			if (m_rxHandler && c->size()) { // size()==0: chunk voided by error recovery
				m_rxHandler(std::span<const uint8_t>{c->data(), c->size()});
			}
			m_rx.pop();
		}

		healthCheck(now_ms);

		// Overflow recovery: if DMA is currently draining into the drop buffer
		// (all chunks were in flight when it was armed), switch back to a real
		// chunk as soon as the consumer has freed one — without waiting for the
		// next IDLE/TC event. Shrinks the loss window from "one full chunk" to
		// "one proceed() period". The bytes already in the drop buffer are lost
		// either way; COBS resynchronizes on the next 0x00 delimiter.
		if (m_started && m_active == nullptr && !m_rx.full()) {
			// The abort runs UNGUARDED — HAL_DMA_Abort polls HAL_GetTick,
			// which freezes under an IRQ guard (same hazard as everywhere
			// else) — and its result is honoured: on HAL_TIMEOUT the transfer
			// is still live, so neither the DMA counter nor any chunk may be
			// touched. Defer to the full restart path instead.
			if (HAL_UART_AbortReceive(m_huart) != HAL_OK) {
				++m_stats.rx_errors;
				m_started = false; // tail of proceed() -> receiveRestart()
			} else {
				IRQGuard guard;
				// An RX ISR may have slipped in before the abort took effect
				// and re-armed onto a real chunk; publish whatever landed in
				// it (per the now-frozen counter) so nothing real is lost.
				if (m_started) { // an error ISR may have deferred a full restart
					publishActive(dmaReceivedCount());
					receiveArm();
				}
			}
		}

		// Receiver not armed (error path hit a busy HAL, or init raced) -> retry.
		if (!m_started) {
			receiveRestart();
		}
	}

	/* -------------------------------- TX -------------------------------- */

	[[nodiscard]] bool tx_busy() const noexcept { return m_txBusy; }

	// Starts DMA on caller-owned memory. The memory is only BORROWED: it must
	// stay valid until tx_busy() returns false (COBS owns the frame, §21–22).
	// Contract: send() is called from ONE execution context (the same loop
	// that runs proceed()); it is not re-entrant against itself.
	bool send(std::span<const uint8_t> bytes) noexcept
	{
		if (m_txBusy || bytes.empty() || !m_huart) {
			return false;
		}

		uart_detail::dcacheClean(bytes.data(), bytes.size());

		// TX deadline adapts to the configured baud rate: ~10 bit-times per
		// byte (start + 8 data + stop) plus a fixed margin. If DMA has not
		// completed by then, healthCheck() aborts the transfer and clears busy.
		{
			const uint32_t baud = m_huart->Init.BaudRate;
			const uint32_t transfer_ms = (baud != 0u)
				? (static_cast<uint32_t>(bytes.size()) * 10000u) / baud
				: 0u;
			m_txDeadline = HAL_GetTick() + transfer_ms + UART_ENGINE_TX_TIMEOUT_MARGIN_MS;
		}

		// Guarded so neither the completion nor the error ISR can observe the
		// half-configured window between starting the DMA and raising the busy
		// flag (an RX-side error ISR in that window would see "busy while
		// gState READY" and falsely fail the transfer). HAL_UART_Transmit_DMA
		// only programs registers — it never blocks — so holding the short
		// guard across it is safe.
		IRQGuard guard;
		if (HAL_UART_Transmit_DMA(m_huart, bytes.data(),
		                          static_cast<uint16_t>(bytes.size())) != HAL_OK) {
			++m_stats.tx_errors;
			return false;
		}
		m_txBusy = true;
		return true;
	}

	[[nodiscard]] const Stats& stats() const noexcept { return m_stats; }
	[[nodiscard]] UART_HandleTypeDef* instance() const noexcept { return m_huart; }

private:
	/* ---------------------------- ISR: RX ------------------------------- */

	// HAL_UARTEx_RxEventCallback path. `size` is the write position inside the
	// current DMA buffer; since every reception starts on a fresh chunk, it is
	// exactly the number of valid bytes in that chunk.
	void isrRxEvent(const uint16_t size) noexcept
	{
		// A stray event delivered while a recovery is pending (m_started
		// cleared, hardware not stopped yet) must not publish anything.
		if (!m_started) {
			return;
		}

#if UART_ENGINE_HAS_RXEVENT_TYPE
		if (HAL_UARTEx_GetRxEventType(m_huart) == HAL_UART_RXEVENT_HT) {
			return; // HT is disabled in receiveArm(); ignore if it slips through
		}
#endif
		// On BOTH event kinds the HAL has already ENDED the reception before
		// invoking this callback — verified in the F1, G4 and H7RS HAL
		// sources:
		//   TC   -> UART_DMAReceiveCplt() clears CR3.DMAR and sets RxState
		//           READY (the DMA finished by itself in normal mode);
		//   IDLE -> HAL_UART_IRQHandler() clears CR3.DMAR, sets RxState READY
		//           and calls HAL_DMA_Abort(hdmarx) itself, and only then
		//           invokes HAL_UARTEx_RxEventCallback.
		// If reception is somehow STILL running (a continuous mode that
		// slipped past init(), or a future HAL change), freeze software
		// ownership and defer the hardware stop to thread context. The
		// chunk is deliberately NOT published: DMA may still be writing
		// into it, and publishing would hand the slot to the consumer and
		// then back to the free pool while hardware still owns it. It stays
		// claimed in m_active, so receiveArm() re-uses that very chunk once
		// receiveRestart() has stopped the DMA for real. Aborting here is
		// not an option either: HAL_UART_AbortReceive() issues
		// UART_RXDATA_FLUSH_REQUEST on the new USART IP, discarding bytes
		// already sitting in RDR/FIFO.
		if (m_huart->RxState != HAL_UART_STATE_READY) {
			++m_stats.rx_errors;
			m_started = false; // proceed() -> receiveRestart(), tick alive
			return;
		}

		__DMB(); // DMA-written data visible before publishing

		// HAL sampled `size` from the DMA counter at IRQ entry, but the DMA
		// kept running until HAL stopped it — bytes it moved into the chunk in
		// that window are already consumed from RDR/FIFO and would be lost
		// silently if the stale value were published. The counter is frozen
		// now (disable never resets CNDTR/NDTR/BNDT), so re-derive the
		// authoritative count from it; on TC it reads 0 and the formula yields
		// exactly ChunkSize. HAL derives its IDLE sizes the same way.
		(void)size;
		publishActive(dmaReceivedCount());
		// note: if m_active == nullptr the bytes went into m_drop — discarded.

		receiveArm();
	}

	// Bytes the DMA has deposited into the current buffer. Authoritative only
	// once the reception is stopped (IDLE-abort or TC) — the count registers
	// persist across channel/stream disable on every DMA IP.
	[[nodiscard]] uint16_t dmaReceivedCount() const noexcept
	{
		return static_cast<uint16_t>(
			ChunkSize - __HAL_DMA_GET_COUNTER(m_huart->hdmarx));
	}

	// Hand the active chunk to the consumer: invalidate, commit the byte
	// count, publish, release ownership. No-op when nothing is claimed
	// (drop mode) or nothing was received.
	void publishActive(const uint16_t size) noexcept
	{
		if (m_active && size) {
			uart_detail::dcacheInvalidate(m_active->data(), size);
			m_active->commit_size(size);
			m_rx.publish();
			m_active = nullptr;
		}
	}

	// Claim the next chunk and re-arm DMA onto it. If the fifo is exhausted
	// (consumer too slow), receive into the drop buffer and count an overrun —
	// COBS resynchronizes on the next 0x00 delimiter (architecture doc §18).
	void receiveArm() noexcept
	{
		// Reuse an already-claimed chunk (a zero-size event, or a previous arm
		// attempt that failed) instead of claiming a second one: a claimed
		// slot that never gets published would leak out of the fifo cycle
		// permanently and shrink the pool.
		if (m_active == nullptr) {
			if ((m_active = m_rx.try_claim()) == nullptr) {
				++m_stats.rx_overrun;
			}
		}
		uint8_t* const dst = (m_active != nullptr) ? m_active->data() : m_drop.data();

		// AN4839: before DMA writes into a cacheable buffer, dirty lines
		// covering it must be discarded, otherwise a later eviction would
		// overwrite freshly received bytes mid-transfer.
		uart_detail::dcacheInvalidate(dst, ChunkSize);

		m_started = (HAL_UARTEx_ReceiveToIdle_DMA(m_huart, dst,
		             static_cast<uint16_t>(ChunkSize)) == HAL_OK);
#ifdef DMA_IT_HT
		// Half-transfer events are not used by the buffer-switching scheme.
		// (Guarded: symbol name differs on some GPDMA-based families; if it is
		// absent, HT events still arrive but are ignored by isrRxEvent.)
		if (m_started) {
			__HAL_DMA_DISABLE_IT(m_huart->hdmarx, DMA_IT_HT);
		}
#endif
	}

	// Full receiver (re)start: abort, clear stale error flags, arm.
	// Thread context only (init / proceed / healthCheck) — never from an ISR.
	void receiveRestart() noexcept
	{
		// The abort runs OUTSIDE the IRQ guard: HAL_DMA_Abort inside polls a
		// timeout with HAL_GetTick(), and the tick freezes while interrupts
		// are masked — a truly wedged DMA would spin forever. In thread
		// context the tick keeps running and the HAL timeout can fire.
		//
		// Its result MUST be honoured: on a DMA-abort timeout the HAL returns
		// HAL_TIMEOUT and the transfer is NOT stopped. Touching or re-arming
		// m_active then would hand a slot to hardware we do not control. Bail
		// out instead, leaving the chunk claimed and unpublished — the next
		// proceed() retries this recovery. Invariant: only ever touch
		// m_active once the DMA is confirmed stopped.
		if (HAL_UART_AbortReceive(m_huart) != HAL_OK) {
			++m_stats.rx_errors;
			m_started = false;
			return;
		}

		IRQGuard guard;

		// Clear stale PE/FE/NE/ORE/IDLE(/RTO) exactly per reference manual:
		// legacy IP -> "read SR then read DR" sequence, new IP -> ICR write.
		// (On legacy IP the DR read may consume one in-flight byte; the
		// restart path is already lossy and COBS resynchronizes on 0x00.)
		UART_ENGINE_CLEAR_RX_ERRORS(m_huart);
		m_huart->ErrorCode = HAL_UART_ERROR_NONE;

		// A chunk still claimed here (e.g. the ISR froze ownership because
		// reception had not stopped) is now safe to touch: the abort above
		// ended the transfer. Drop its unreliable content and let
		// receiveArm() re-arm DMA into that same slot — no publish, no leak.
		if (m_active) {
			m_active->commit_size(0);
		}

		++m_stats.restarts;
		receiveArm();
		__DSB(); __ISB();
	}

	/* ---------------------------- ISR: TX ------------------------------- */

	void isrTxCplt() noexcept
	{
		// Report the real outcome, not blind success (portable: gState and the
		// TX DMA error code exist on every series with DMA support).
		bool ok = true;
		const HAL_UART_StateTypeDef gState = HAL_UART_GetState(m_huart);
		if (gState == HAL_UART_STATE_ERROR || gState == HAL_UART_STATE_TIMEOUT) {
			ok = false;
		}
		if (m_huart->hdmatx && HAL_DMA_GetError(m_huart->hdmatx) != HAL_DMA_ERROR_NONE) {
			ok = false;
		}

		__DMB();
		m_txBusy = false;
		if (!ok) {
			++m_stats.tx_errors;
		}
		if (m_txHandler) {
			m_txHandler(ok);
		}
	}

	/* --------------------------- ISR: error ----------------------------- */

	// Error callback — runs in ISR context, therefore it must NOT call any
	// blocking HAL abort (HAL_DMA_Abort polls HAL_GetTick, which is frozen
	// while an interrupt is being serviced). By the time HAL invokes this
	// callback it has already done the transfer-level cleanup itself:
	//  - blocking RX errors (ORE, DMA): HAL ended the reception -> RxState READY;
	//  - non-blocking line noise (PE/FE/NE): reception KEEPS RUNNING — the
	//    corrupted bytes stay in the stream and are filtered by CRC/COBS above,
	//    so killing and restarting the receiver here would only lose data;
	//  - TX-side error: HAL ended the transmission -> gState READY.
	// This ISR therefore only classifies, releases ownership and defers the
	// actual re-arm to proceed() (m_started = false), where HAL timeouts work.
	void isrError() noexcept
	{
		const uint32_t errorCode = HAL_UART_GetError(m_huart);

#if UART_ENGINE_NEW_USART_IP
		// ICR write-1-to-clear: side-effect free, always safe.
		UART_ENGINE_CLEAR_RX_ERRORS(m_huart);
#else
		// Legacy IP clears via "read SR then read DR" — the DR read would
		// STEAL a live data byte from the RX DMA stream, so clear only when
		// the reception is already dead. (While it runs, HAL's own IRQ
		// handler has performed the clearing sequence before calling us.)
		if (m_huart->RxState == HAL_UART_STATE_READY) {
			UART_ENGINE_CLEAR_RX_ERRORS(m_huart);
		}
#endif
		m_huart->ErrorCode = HAL_UART_ERROR_NONE;

		const HAL_UART_StateTypeDef gState = HAL_UART_GetState(m_huart);
		const bool deadState = (gState == HAL_UART_STATE_ERROR) ||
		                       (gState == HAL_UART_STATE_TIMEOUT);

		// RX: reception ended (blocking error) or the peripheral is dead ->
		// the chunk DMA was filling holds an unknown/unreliable prefix: void
		// it and let proceed() re-arm from thread context.
		if (deadState || m_huart->RxState == HAL_UART_STATE_READY) {
			++m_stats.rx_errors;
			voidActiveChunk();
			m_started = false; // proceed() -> receiveRestart() with live tick
		} else if (errorCode & UART_ENGINE_RX_PART) {
			++m_stats.rx_errors; // line noise; reception still running
		}

		// TX: HAL ended the transmission on a TX-side error -> release the
		// borrowed frame so the layer above can free or retry it.
		if (m_txBusy && (deadState || gState == HAL_UART_STATE_READY)) {
			m_txBusy = false;
			++m_stats.tx_errors;
			if (m_txHandler) {
				m_txHandler(false);
			}
		}

		if (m_errHandler && errorCode != HAL_UART_ERROR_NONE) {
			m_errHandler(errorCode);
		}
	}

	// Publish the currently claimed chunk with size 0: its content is
	// unreliable after an error, and publishing (rather than dropping the
	// pointer) returns the slot through the normal fifo cycle — no leak.
	void voidActiveChunk() noexcept
	{
		if (m_active) {
			m_active->commit_size(0);
			m_rx.publish();
			m_active = nullptr;
		}
	}

	/* ----------------------- watchdog (main loop) ----------------------- */

	// Periodic self-care: detects silently dead receivers, locked DMA streams
	// and stuck TX transfers on any STM32 series, and recovers without user
	// involvement. Runs in thread context from proceed().
	void healthCheck(const uint32_t now_ms) noexcept
	{
		// --- TX deadline: DMA completion interrupt never arrived ---------
		if (m_txBusy && static_cast<int32_t>(now_ms - m_txDeadline) > 0) {
			// Abort outside the IRQ guard (HAL_DMA_Abort polls HAL_GetTick,
			// which is frozen under a guard). Aborting a transfer that just
			// completed is harmless; the guarded re-check below keeps the
			// bookkeeping consistent if the completion ISR won the race.
			//
			// The result is honoured for the TX mirror of the RX invariant:
			// the caller's span is only released once the DMA is confirmed
			// stopped. On HAL_TIMEOUT the transfer is still reading that
			// memory, so tx_busy() must STAY true — clearing it would invite
			// the layer above to free or reuse a buffer hardware still owns.
			// The periodic audit below then escalates to a full recovery.
			if (HAL_UART_AbortTransmit(m_huart) != HAL_OK) {
				++m_stats.tx_errors;
			} else {
				IRQGuard guard;
				if (m_txBusy) {
					m_txBusy = false;
					++m_stats.tx_errors;
					if (m_txHandler) {
						m_txHandler(false);
					}
				}
			}
		}

		// --- periodic peripheral state audit ------------------------------
		if ((now_ms - m_lastCheckTime) < UART_ENGINE_CHECK_PERIOD_MS) {
			return;
		}
		m_lastCheckTime = now_ms;

		const HAL_UART_StateTypeDef gState = HAL_UART_GetState(m_huart);
		const uint32_t errorCode = HAL_UART_GetError(m_huart);

		bool bad = (gState == HAL_UART_STATE_ERROR) ||
		           (gState == HAL_UART_STATE_TIMEOUT) ||
		           (errorCode != HAL_UART_ERROR_NONE);

		// RX should be actively receiving whenever we believe it is armed.
		// RxState is present on every HAL recent enough to have ReceiveToIdle.
		if (!bad && m_started && m_huart->RxState == HAL_UART_STATE_READY) {
			bad = true; // receiver silently stopped (missed error IRQ, aborted elsewhere)
		}

		// DMA stream/channel error codes (DMA, BDMA, GPDMA — all report
		// through HAL_DMA_GetError).
		if (!bad && m_huart->hdmarx &&
				HAL_DMA_GetError(m_huart->hdmarx) != HAL_DMA_ERROR_NONE) {
			bad = true;
		}
		if (!bad && m_txBusy && m_huart->hdmatx &&
				HAL_DMA_GetError(m_huart->hdmatx) != HAL_DMA_ERROR_NONE) {
			bad = true;
		}

		if (!bad) {
			m_failCounter = 0;
			return;
		}

		// Debounce: recover only after N consecutive bad checks, so a transient
		// state observed mid-IRQ does not cause a spurious restart.
		if (++m_failCounter >= UART_ENGINE_FAIL_THRESHOLD) {
			m_failCounter = 0;

			// Blocking HAL aborts run with interrupts enabled (tick alive, so
			// the HAL's own timeouts remain functional); only the state
			// mutation is IRQ-guarded. Both the RX and TX invariants apply:
			// release nothing until the transfers are confirmed stopped, or
			// voidActiveChunk() would publish a slot DMA still writes and
			// tx_busy() would free a span DMA still reads.
			(void)HAL_UART_DMAStop(m_huart);
			if (HAL_UART_Abort(m_huart) != HAL_OK) {
				++m_stats.rx_errors;
				m_started = false; // retry from proceed() on every iteration
				return;
			}
			{
				IRQGuard guard;
				voidActiveChunk();
				m_txBusy = false;
			}
			receiveRestart();
		}
	}

private:
	/*
	 * RX chunk fifo: storage is INLINE — the whole Uart object must be placed
	 * in DMA-accessible RAM by the user (see file header).
	 */
	RxFifo m_rx{};
	RxChunk* m_active = nullptr; // chunk currently owned by DMA (claimed, not published)

	// Fallback DMA target when all chunks are in flight; contents are discarded.
	alignas(32) std::array<uint8_t, ChunkSize> m_drop{};

	UART_HandleTypeDef* m_huart = nullptr;
	volatile bool m_started = false;
	volatile bool m_txBusy  = false;

	RxHandler    m_rxHandler{};
	TxHandler    m_txHandler{};
	ErrorHandler m_errHandler{};

	// Watchdog state (thread context only).
	uint32_t m_lastCheckTime = 0;
	uint32_t m_txDeadline    = 0;
	uint8_t  m_failCounter   = 0;

	Stats m_stats{};
};

/* ===================== HAL callbacks (internal variant) =================== */
/*
 * Define UART_ENGINE_IMPLEMENT in exactly ONE .cpp before including Uart.h
 * (only needed when UART_ENGINE_INTERNAL_CALLBACKS_ON == 1 and the HAL
 * register-callbacks feature is off).
 */
#if UART_ENGINE_INTERNAL_CALLBACKS_ON && !(USE_HAL_UART_REGISTER_CALLBACKS == 1)
#ifdef UART_ENGINE_IMPLEMENT

extern "C" void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t Size)
{
	UartRegistry::onRxEvent(huart, Size);
}

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart)
{
	UartRegistry::onTxCplt(huart);
}

extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart)
{
	UartRegistry::onError(huart);
}

#endif /* UART_ENGINE_IMPLEMENT */
#endif /* internal callbacks */

#endif /* HAL_UART_MODULE_ENABLED */
#endif /* UART_ENGINE_UART_H_ */
