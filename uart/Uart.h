/*
 * Uart.h — zero-copy STM32 UART byte transport (always-DMA, buffer switching)
 *
 * Current contract: this header plus uart/tests/host and uart/tests/port.
 * Historical rationale only: doc/old/UART_COBS_ARCHITECTURE.md, §3–§8.
 *  - One mode only: DMA normal (non-circular) via HAL_UARTEx_ReceiveToIdle_DMA.
 *  - RX: spsc::cache_aligned_chunk_fifo — DMA writes directly into a claimed
 *    chunk slot; the ISR only commits the size, publishes the chunk and
 *    re-arms DMA onto the next free slot. Zero memcpy, zero heap.
 *  - TX: no queue. send(span) starts DMA on caller-owned memory, tx_busy()
 *    reports completion. Policy on Busy belongs to the layer above (COBS).
 *  - Portable across STM32 series: register differences live in
 *    detail/UartRegs.h,
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

#include "detail/IrqGuard.h"
#include "detail/UartRegs.h" // SR/DR vs ISR/RDR abstraction (all STM32 series)
#include "uart_probe.h"      // benchmark instrumentation; compiles to nothing by default

#include "spsc/chunk_fifo.hpp"   // needs both the SPSC root and src include paths
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
// 0 -> the user forwards their own callbacks to
//      uart::detail::Registry::onRxEvent/onTxCplt/onError.
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

// NOTE on TX liveness: there is no per-frame deadline. A transfer is judged
// by whether the hardware DEMONSTRATES PROGRESS, checked in the same periodic
// audit as everything else — see healthCheck(). That is frame-length
// independent (a 64 KB frame is treated exactly like an 8-byte one), needs no
// baud-rate arithmetic in send(), and asks the honest question: is the
// hardware moving, or is it dead? It is still a timeout in the end, just
// expressed as "must make progress within N audit periods" rather than "must
// finish within X milliseconds".

// The IDLE/HT/TC events themselves are HARDWARE features present on every
// STM32 (IDLE flag on all USART IPs, HT/TC on all DMA/BDMA/GPDMA types).
// But HAL_UARTEx_GetRxEventType() and the HAL_UART_RXEVENT_* enum are a HAL
// add-on introduced LATER than ReceiveToIdle itself, per-family; all current
// packages have them, an older cached HAL may not. They are enum values (not
// #defines), so presence cannot be auto-detected by the preprocessor — set
// this to 0 if the compiler reports HAL_UARTEx_GetRxEventType undeclared:
// the event kind is then derived from RxState (READY -> TC, still
// receiving -> IDLE/HT), which is equivalent for this driver.
#if defined(__GNUC__)
#	define UART_ENGINE_NOINLINE __attribute__((noinline))
#else
#	define UART_ENGINE_NOINLINE
#endif

#ifndef UART_ENGINE_HAS_RXEVENT_TYPE
#	define UART_ENGINE_HAS_RXEVENT_TYPE 1
#endif

/* ============================ D-cache helpers ============================= */

namespace uart::detail {

#if UART_ENGINE_DCACHE_MAINTENANCE

static inline bool dcache_enabled() noexcept
{
	return (SCB->CCR & SCB_CCR_DC_Msk) != 0u;
}

static inline void invalidate_dcache(const void* const addr, const uint32_t size) noexcept
{
	if (addr && size && dcache_enabled()) {
		SCB_InvalidateDCache_by_Addr(const_cast<void*>(addr), static_cast<int32_t>(size));
	}
}

static inline void clean_dcache(const void* const addr, const uint32_t size) noexcept
{
	if (addr && size && dcache_enabled()) {
		SCB_CleanDCache_by_Addr(const_cast<void*>(addr), static_cast<int32_t>(size));
	}
}

#else /* stubs — no D-cache on this core, or MPU region is non-cacheable */

static inline void invalidate_dcache(const void*, const uint32_t) noexcept {}
static inline void clean_dcache(const void*, const uint32_t) noexcept {}

#endif /* UART_ENGINE_DCACHE_MAINTENANCE */

/* ============================ instance registry =========================== */

// Non-template, non-virtual dispatch from HAL callbacks to instances.
// Fixed-capacity static table: no heap, no std::vector, no vtables.
// The linear search runs over <= UART_ENGINE_MAX_INSTANCES entries per event.
class Registry final {
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
		IrqGuard guard;
		for (const auto& e : s_entries) {
			// Reject both a duplicate handle AND a second handle that drives
			// the same physical peripheral: two engines on one USARTx would
			// arm DMA over each other while each believed it owned the line.
			if (e.huart == huart ||
					(e.huart != nullptr && e.huart->Instance == huart->Instance)) {
				return false; // one engine per UART
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
		IrqGuard guard;
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
	// Registry, which C++ forbids for the in-class definition.
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

} // namespace uart::detail

/* ================================ engine ================================== */

// Defaults chosen from the H7S bench (10 Mbaud, D-cache on, cycles per useful
// received byte — the whole engine, IRQs included):
//     128x4 18.72   128x8 18.93   256x4 19.39   512x4 20.81
// The per-event cost is dominated by invalidating the WHOLE chunk before
// arming, so it grows with ChunkSize while the event rate falls more slowly:
// smaller chunks win. 8 chunks rather than 4 keeps the pool at the same 1 KB
// of buffering the old 256x4 default provided — halving ChunkSize alone would
// silently halve every application's overflow headroom.
// Raise ChunkSize for a genuinely continuous stream (each TC then replaces
// several IDLE events); lower ChunkCount only if RAM is scarce.
template<std::size_t ChunkSize = 128, std::size_t ChunkCount = 8>
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
	// Raised when bytes were physically lost from the stream (the chunk pool
	// ran dry and DMA drained into the drop buffer). Delivered in ORDER,
	// between the last byte received before the loss and the first one after
	// it, so a decoder above can abandon the frame it was assembling instead
	// of silently joining two halves that never touched. COBS framing alone
	// cannot detect this: the bytes surrounding a gap may well form a
	// structurally valid frame.
	using GapHandler   = tiny::delegate<void()>;

	// Diagnostics, not accounting: these are plain counters incremented from
	// both ISR and thread context, so concurrent increments can lose a count,
	// and stats() hands out a live reference rather than a coherent snapshot.
	// They are meant for sizing (is rx_overrun non-zero?), not for arithmetic
	// anyone depends on. Atomics are deliberately not paid for here.
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
			uart::detail::Registry::detach(this);
			AllTeardown ts(*this);
			(void)HAL_UART_Abort(m_huart);
			m_huart = nullptr;
		}
	}

	/* ------------------------------ setup ------------------------------- */

	// Pre-flight validates EVERYTHING the runtime relies on; a false return
	// means the CubeMX/startup configuration is wrong, not a transient error.
	//
	// A Uart object binds to exactly ONE handle, exactly once: re-binding a
	// live engine to a second UART would leave the first peripheral's DMA
	// writing into this object's chunk storage while the second one is armed
	// onto the very same memory. To drive another UART, use another object.
	// (A failed init leaves the object unbound, so it may be retried.)
	bool init(UART_HandleTypeDef* const huart) noexcept
	{
		if (m_huart != nullptr) {
			return false; // already bound — see the one-shot contract above
		}

		/* --- structural checks ------------------------------------------ */
		if (!huart || !huart->Instance || !huart->hdmarx || !huart->hdmatx) {
			return false; // no handle, no peripheral, or DMA not linked (CubeMX)
		}
		// RESET  -> HAL_UART_Init() was never run.
		// BUSY_*  -> somebody is already driving this peripheral. This engine
		//            takes exclusive ownership and would abort their transfer
		//            on the first receiveRestart(), so refuse to steal it.
		if (huart->gState != HAL_UART_STATE_READY ||
				huart->RxState != HAL_UART_STATE_READY) {
			return false;
		}
		if (huart->hdmarx->State != HAL_DMA_STATE_READY ||
				huart->hdmatx->State != HAL_DMA_STATE_READY) {
			return false; // HAL_DMA_Init() never run, or a transfer is live
		}
		// __HAL_LINKDMA() sets the back-pointer that every HAL DMA callback
		// casts and dereferences (UART_DMAReceiveCplt, UART_DMAError, ...).
		// A handle wired up by hand, or linked to a different UART, would
		// fault or deliver this peripheral's events to another driver at the
		// first transfer — from inside an ISR, where it is hardest to debug.
		if (huart->hdmarx->Parent != huart || huart->hdmatx->Parent != huart) {
			return false;
		}
		// RX and TX must be distinct hardware channels: one channel cannot
		// serve two directions, and the second Start would reprogram the
		// first transfer out from under it.
		if (huart->hdmarx == huart->hdmatx ||
				huart->hdmarx->Instance == huart->hdmatx->Instance) {
			return false;
		}
		// Transfer geometry. A wrong direction moves data the wrong way; a
		// missing memory increment rewrites one byte in place; and — the
		// nasty one — an enabled PERIPHERAL increment walks the DMA across
		// the registers neighbouring RDR/TDR.
		if (huart->hdmarx->Init.Direction != DMA_PERIPH_TO_MEMORY ||
				huart->hdmatx->Init.Direction != DMA_MEMORY_TO_PERIPH) {
			return false;
		}
#if defined(DMA_PINC_DISABLE)
		if (huart->hdmarx->Init.PeriphInc != DMA_PINC_DISABLE ||
				huart->hdmatx->Init.PeriphInc != DMA_PINC_DISABLE ||
				huart->hdmarx->Init.MemInc != DMA_MINC_ENABLE ||
				huart->hdmatx->Init.MemInc != DMA_MINC_ENABLE) {
			return false;
		}
#elif defined(DMA_SINC_FIXED)
		// GPDMA names the sides source/destination: on RX memory is the
		// destination, on TX it is the source.
		if (huart->hdmarx->Init.SrcInc != DMA_SINC_FIXED ||
				huart->hdmarx->Init.DestInc != DMA_DINC_INCREMENTED ||
				huart->hdmatx->Init.SrcInc != DMA_SINC_INCREMENTED ||
				huart->hdmatx->Init.DestInc != DMA_DINC_FIXED) {
			return false;
		}
#endif
		// BOTH directions require a one-shot DMA; a continuous mode breaks a
		// different ownership contract on each side:
		//
		//   RX — reception must END when the buffer fills or the line goes
		//        idle, or a published chunk keeps being written behind the
		//        consumer's back. (Plain linked-list mode is refused too:
		//        HAL_DMA_Abort() zeroes CBR1 for it, which would make
		//        dmaReceivedCount() report a full chunk of stale bytes.)
		//
		//   TX — in circular mode UART_DMATransmitCplt() invokes
		//        HAL_UART_TxCpltCallback at the end of EVERY lap while the
		//        DMA keeps reading and re-sending the frame. tx_busy() would
		//        drop to false, the layer above would free or reuse a buffer
		//        hardware is still transmitting from, and the line would loop
		//        that memory forever.
		//
		// The continuous mode is spelled differently per DMA IP:
#ifdef DMA_CIRCULAR
		// Classic channel/stream DMA (F0/F1/F3/F4/F7/G0/G4/L4/H7 DMA1-2 ...).
		if (huart->hdmarx->Init.Mode == DMA_CIRCULAR ||
				huart->hdmatx->Init.Mode == DMA_CIRCULAR) {
			return false;
		}
#endif
#ifdef DMA_LINKEDLIST
		// GPDMA/HPDMA (H5, H7RS, U5, WBA ...): DMA_CIRCULAR does not exist
		// there; the continuous variant is DMA_LINKEDLIST_CIRCULAR, which the
		// UART HAL itself tests before deciding whether to end a transfer.
		if ((huart->hdmarx->Mode & DMA_LINKEDLIST) != 0u ||
				(huart->hdmatx->Mode & DMA_LINKEDLIST) != 0u) {
			return false;
		}
#endif
		if (huart->Init.BaudRate == 0u) {
			return false; // an initialized handle with no baud rate is broken
		}
		// COBS uses the whole 0x00..0xFF range, so the frame must carry EIGHT
		// payload bits. WordLength counts the parity bit as one of its data
		// bits, so "8B + parity" actually transports only 7 payload bits and
		// would silently mangle the encoding. Exactly two configurations
		// qualify: 8 bits without parity, or 9 bits with it.
		{
			const bool parity = (huart->Init.Parity != UART_PARITY_NONE);
			const bool eight_bit_payload =
				((huart->Init.WordLength == UART_WORDLENGTH_8B) && !parity) ||
				((huart->Init.WordLength == UART_WORDLENGTH_9B) && parity);
			if (!eight_bit_payload) {
				return false;
			}
		}
		// The engine keeps RX permanently armed while allowing TX, so both
		// directions must be enabled...
		if (huart->Init.Mode != UART_MODE_TX_RX) {
			return false;
		}
#ifdef USART_CR3_HDSEL
		// ...and the line must not be shared: in half-duplex the transmitter
		// drives the very wire our receiver is listening on.
		if (READ_BIT(huart->Instance->CR3, USART_CR3_HDSEL) != 0u) {
			return false;
		}
#endif
		// The DMA must move BYTES. HAL programs the transfer as a count of
		// ELEMENTS (ChunkSize), so a half-word memory width would make the
		// controller write 2 * ChunkSize bytes into a ChunkSize buffer and
		// run straight off the end of the chunk — a real overflow, typically
		// reached by configuring 9-bit data or by a CubeMX slip.
#if defined(DMA_MDATAALIGN_BYTE)
		// Classic channel/stream DMA: the memory side is the chunk on RX and
		// the caller's frame on TX.
		if (huart->hdmarx->Init.MemDataAlignment != DMA_MDATAALIGN_BYTE ||
				huart->hdmatx->Init.MemDataAlignment != DMA_MDATAALIGN_BYTE ||
				huart->hdmarx->Init.PeriphDataAlignment != DMA_PDATAALIGN_BYTE ||
				huart->hdmatx->Init.PeriphDataAlignment != DMA_PDATAALIGN_BYTE) {
			return false;
		}
#elif defined(DMA_DEST_DATAWIDTH_BYTE)
		// GPDMA/HPDMA: memory is the destination on RX, the source on TX.
		// memory side: RX destination, TX source; peripheral side: the other
		// two. All four must be byte-wide.
		if (huart->hdmarx->Init.DestDataWidth != DMA_DEST_DATAWIDTH_BYTE ||
				huart->hdmarx->Init.SrcDataWidth != DMA_SRC_DATAWIDTH_BYTE ||
				huart->hdmatx->Init.SrcDataWidth != DMA_SRC_DATAWIDTH_BYTE ||
				huart->hdmatx->Init.DestDataWidth != DMA_DEST_DATAWIDTH_BYTE) {
			return false;
		}
#endif

		/* --- registration ------------------------------------------------ */
		// Ownership progression: validate -> registry attach -> callbacks ->
		// claim m_huart -> start hardware. m_huart stays null until this
		// object is the confirmed owner, so a rejected init leaves nothing
		// behind — in particular the destructor has no handle to abort, and
		// cannot tear down a peripheral that belongs to another instance.
		static constexpr uart::detail::Registry::Ops ops = {
			[](void* self, uint16_t n) { static_cast<Uart*>(self)->isrRxEvent(n); },
			[](void* self)             { static_cast<Uart*>(self)->isrTxCplt(); },
			[](void* self)             { static_cast<Uart*>(self)->isrError(); },
		};
		{
			// One guarded step: an ISR can never observe this object as
			// registered-but-unbound (m_huart still null), which isrError()
			// would dereference.
			uart::detail::IrqGuard guard;
			if (!uart::detail::Registry::attach(huart, this, ops)) {
				return false; // table full, or this UART is already owned
			}
			m_huart = huart;
		}

		// Under CTS flow control the peer may legitimately keep the DMA from
		// advancing, so a frozen counter proves nothing and STALL detection
		// is disabled. Detection of a LOST COMPLETION stays on even then —
		// see healthCheck(). Decided once, here: the configuration cannot
		// change while this object stays bound.
		m_txProgressWatchEnabled = (huart->Init.HwFlowCtl != UART_HWCONTROL_CTS) &&
		                           (huart->Init.HwFlowCtl != UART_HWCONTROL_RTS_CTS);

#if (USE_HAL_UART_REGISTER_CALLBACKS == 1)
		bool reg_ok = (HAL_UART_RegisterRxEventCallback(huart,
			[](UART_HandleTypeDef* h, uint16_t n) { uart::detail::Registry::onRxEvent(h, n); }) == HAL_OK);
		reg_ok = reg_ok && (HAL_UART_RegisterCallback(huart, HAL_UART_TX_COMPLETE_CB_ID,
			[](UART_HandleTypeDef* h) { uart::detail::Registry::onTxCplt(h); }) == HAL_OK);
		reg_ok = reg_ok && (HAL_UART_RegisterCallback(huart, HAL_UART_ERROR_CB_ID,
			[](UART_HandleTypeDef* h) { uart::detail::Registry::onError(h); }) == HAL_OK);
		if (!reg_ok) {
			uart::detail::IrqGuard guard;
			uart::detail::Registry::detach(this);
			m_huart = nullptr;
			return false;
		}
#endif

		receiveRestart();
		if (!m_started) {
			// Hardware refused to start: give the handle back rather than
			// staying half-bound. Registered HAL callbacks (if any) are left
			// in place — without a registry entry they resolve to no-ops.
			uart::detail::Registry::detach(this);
			m_huart = nullptr;
			return false;
		}
		return true;
	}

	// IRQ-guarded: a handler may be (re)assigned even while reception is
	// already running — the ISR never observes a half-written delegate.
	void setRxHandler(RxHandler h) noexcept
	{
		uart::detail::IrqGuard guard;
		m_rxHandler = static_cast<RxHandler&&>(h);
	}
	void setTxHandler(TxHandler h) noexcept
	{
		uart::detail::IrqGuard guard;
		m_txHandler = static_cast<TxHandler&&>(h);
	}
	void setErrorHandler(ErrorHandler h) noexcept
	{
		uart::detail::IrqGuard guard;
		m_errHandler = static_cast<ErrorHandler&&>(h);
	}
	void setRxGapHandler(GapHandler h) noexcept
	{
		uart::detail::IrqGuard guard;
		m_gapHandler = static_cast<GapHandler&&>(h);
	}

	// Change the line speed of an already-initialised engine, typically once
	// at start-up right after init(). This driver keeps NO copy of the
	// configuration: huart->Init is the single source of truth, so anything
	// else the new rate needs — OverSampling for the very high rates of the
	// newer IP — is set by the application there beforehand.
	//
	// Thread context only (the loop that runs proceed()), and never with a
	// frame in flight: reprogramming BRR under a live TX would corrupt it.
	// Reception is torn down and restarted, so the layer above sees a gap and
	// never merges bytes received at the old speed into the new stream.
	//
	// Returns false — with the PREVIOUS speed restored and running — when the
	// engine is unbound, the rate is zero or unreachable for this kernel
	// clock, or a transmission is in progress.
	bool setBaudRate(const uint32_t baud) noexcept
	{
		if (m_huart == nullptr || baud == 0u || m_txBusy) {
			return false;
		}

		// Reception must be stopped before the peripheral is reprogrammed:
		// HAL_UART_Init disables the UART and rewrites CR1/CR2/CR3 while the
		// DMA would still be writing into a claimed chunk. Same rule as
		// receiveRestart(): the abort runs outside an IRQ guard (it polls
		// HAL_GetTick), and a failed abort means the transfer is NOT stopped,
		// so nothing may be touched — leave the old speed and let proceed()
		// retry the recovery.
		{
			RxTeardown ts(*this);
			if (HAL_UART_AbortReceive(m_huart) != HAL_OK) {
				++m_stats.rx_errors;
				m_started = false;
				return false;
			}
		}

		// HAL_UART_Init only runs MspInit and resets the registered callbacks
		// when gState is RESET (verified in the ST sources for every series):
		// bound and READY, this reconfigures the peripheral alone — GPIO,
		// DMA, NVIC and our callbacks all survive.
#if defined(USART_CR1_FIFOEN)
		// ...but it does silently drop the FIFO: FIFOEN and both threshold
		// fields are inside the CR1/CR3 masks it clears and are never in the
		// value written back, while huart->FifoMode keeps claiming the mode
		// is on. Save the truth from the registers (no shadow copy in this
		// object) and put the hardware back in agreement afterwards.
		const uint32_t fifo_on = m_huart->Instance->CR1 & USART_CR1_FIFOEN;
		const uint32_t tx_thr  = m_huart->Instance->CR3 & USART_CR3_TXFTCFG;
		const uint32_t rx_thr  = m_huart->Instance->CR3 & USART_CR3_RXFTCFG;
#endif
		const uint32_t previous = m_huart->Init.BaudRate;
		m_huart->Init.BaudRate = baud;
		const bool ok = (HAL_UART_Init(m_huart) == HAL_OK);
		if (!ok) {
			// An unreachable rate leaves the peripheral half-configured and
			// disabled. Put the working speed back so a bad request costs a
			// gap, not the link.
			m_huart->Init.BaudRate = previous;
			(void)HAL_UART_Init(m_huart);
		}
#if defined(USART_CR1_FIFOEN)
		if (fifo_on != 0u) {
			(void)HAL_UARTEx_EnableFifoMode(m_huart);
			(void)HAL_UARTEx_SetTxFifoThreshold(m_huart, tx_thr);
			(void)HAL_UARTEx_SetRxFifoThreshold(m_huart, rx_thr);
		}
#endif
		// Voids the stale chunk (the gap the decoder needs), clears the error
		// flags the speed change may have raised on the line, and re-arms.
		receiveRestart();
		return ok && m_started;
	}

	/* ---------------------------- main loop ----------------------------- */

	// Drains published RX chunks and runs the self-healing watchdog. The span
	// handed to the handler is valid ONLY during the callback (the slot
	// returns to the DMA producer on pop). Call this from the main loop.
	// Hot path: a handful of instructions when there is nothing to do. It
	// deliberately does NOT touch the SPSC queue — reading it would take the
	// consumer-side acquire (a dmb on Cortex-M) on every single iteration.
	// A plain doorbell flag, raised by the ISR whenever it actually leaves
	// work behind, answers the same question for free. The race is benign:
	// a publication landing just after the flag is cleared simply raises it
	// again, so the worst case is one redundant slow pass, never missed work.
	//
	// Pass now_ms explicitly from the main loop — the default argument
	// evaluates HAL_GetTick() at the CALL SITE, so `uart.proceed()` pays for
	// a tick read on every iteration even when it returns immediately.
	void proceed(const uint32_t now_ms = HAL_GetTick()) noexcept
	{
		if (m_huart == nullptr) {
			return; // not initialized (or init() failed) — nothing to service
		}
		const bool audit_due =
			(now_ms - m_lastCheckTime) >= UART_ENGINE_CHECK_PERIOD_MS;
		if (!m_rxWorkPending && m_started && !audit_due) {
			return;
		}
		proceedSlow(now_ms, audit_due);
	}

private:
	UART_ENGINE_NOINLINE void proceedSlow(const uint32_t now_ms, const bool audit_due) noexcept
	{
		UART_ENGINE_PROBE_SCOPE(Slow);
		if (m_rxWorkPending) {
			uart::detail::IrqGuard guard;
			m_rxWorkPending = false;
		}

		// Discontinuities are ordered STRUCTURALLY, never by counting:
		//  - a zero-length chunk IS an in-band marker, published by every
		//    path that throws away a partly filled buffer (error recovery,
		//    restart), so it arrives in the queue exactly where the bytes
		//    were lost;
		//  - an overflow gap cannot be published (the queue was full), so it
		//    is flagged instead, and the producer stays in the drop buffer
		//    until announced — hence nothing received after it can be queued
		//    ahead of the announcement below.
		while (RxChunk* const c = m_rx.try_front()) {
			if (c->size() == 0u) {
				reportGap();
			} else if (m_rxHandler) {
				m_rxHandler(std::span<const uint8_t>{c->data(), c->size()});
			}
			m_rx.pop();
		}
		announceGap();

		if (audit_due) {
			m_lastCheckTime = now_ms;
			healthCheck();
		}

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
			// The gate covers ONLY the abort. receiveArm() below starts a NEW
			// transfer, and with a small ChunkSize at multi-Mbaud its first
			// IDLE/TC event can land almost immediately — inside a still-open
			// gate it would be mistaken for a leftover of the old transfer
			// and dropped. Principle: a teardown gate protects the teardown,
			// never what comes after it.
			HAL_StatusTypeDef abort_status;
			{
				RxTeardown ts(*this);
				abort_status = HAL_UART_AbortReceive(m_huart);
				if (abort_status != HAL_OK) {
					// The old transfer may still be alive. Mark the receiver
					// down BEFORE the gate drops: otherwise its callback slips
					// through (both !m_started and the gate still look fine),
					// re-arms, and the restart below then kills that brand-new
					// transfer — a stream hole for nothing.
					m_started = false; // tail of proceed() -> receiveRestart()
				}
			}
			if (abort_status != HAL_OK) {
				++m_stats.rx_errors;
			} else {
				uart::detail::IrqGuard guard;
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

public:
	/* -------------------------------- TX -------------------------------- */

	[[nodiscard]] bool tx_busy() const noexcept { return m_txBusy; }

	// Starts DMA on caller-owned memory. The memory is only BORROWED: it must
	// stay valid until tx_busy() returns false (COBS owns the frame, §21–22).
	// Contract: send() is called from ONE execution context (the same loop
	// that runs proceed()); it is not re-entrant against itself.
	bool send(std::span<const uint8_t> bytes) noexcept
	{
		UART_ENGINE_PROBE_SCOPE(TxStart);
		if (m_txBusy || bytes.empty() || !m_huart) {
			return false;
		}
		// The HAL transfer length is a uint16_t: silently truncating here
		// would transmit a wrong, shorter frame and report success.
		if (bytes.size() > 65535u) {
			++m_stats.tx_errors;
			return false;
		}

		uart::detail::clean_dcache(bytes.data(), bytes.size());

		// No deadline arithmetic here at all: the periodic audit judges this
		// transfer by progress. Just forget what the previous frame looked
		// like so the first observation of this one establishes a baseline.
		m_txProgressValid = false;
		m_txStallChecks = 0u;

		// The guard MUST span the HAL call: the busy flag and the hardware
		// state have to become visible to the ISRs at the same instant.
		// Neither unguarded ordering works —
		//   - flag AFTER the call: for a moment the DMA is already reading the
		//     caller's buffer while m_txBusy still says idle, so a completion
		//     or error arriving there is discarded by the ISRs' !m_txBusy test
		//     and the frame hangs until the TX deadline;
		//   - flag BEFORE the call is worse: an error ISR would see a busy
		//     flag while gState is still READY, conclude the transfer had
		//     ended, release the frame and report failure — just as the DMA
		//     starts reading that very buffer.
		// HAL_UART_Transmit_DMA only programs registers (it never blocks), so
		// the masked window is bounded. The explicit block keeps that window
		// visible at a glance, and leaves the statistics update outside it.
		bool started;
		{
			uart::detail::IrqGuard guard;
			started = (HAL_UART_Transmit_DMA(m_huart, bytes.data(),
			                                 static_cast<uint16_t>(bytes.size())) == HAL_OK);
			if (started) {
#ifdef DMA_IT_HT
				// Mirror of the RX side: this driver exposes no half-TX
				// event, yet the HAL enables HT on every start — measured on
				// H7S as exactly 2 DMA IRQs per frame instead of 1.
				__HAL_DMA_DISABLE_IT(m_huart->hdmatx, DMA_IT_HT);
#endif
				m_txBusy = true;
			}
		}
		if (!started) {
			++m_stats.tx_errors;
		}
		return started;
	}

	[[nodiscard]] const Stats& stats() const noexcept { return m_stats; }
	[[nodiscard]] UART_HandleTypeDef* instance() const noexcept { return m_huart; }

private:
	// RAII: raises the teardown gate for the duration of a HAL abort. Thread
	// context only — the counter is only ever mutated here.
	template<bool Rx, bool Tx>
	class TeardownScope final {
	public:
		// Explicit load/store, not ++/--: a read-modify-write on a
		// volatile-qualified operand is deprecated in C++20 and removed in
		// later drafts.
		explicit TeardownScope(Uart& u) noexcept : m_u(u)
		{
			if constexpr (Rx) { m_u.m_rxTeardown = static_cast<uint8_t>(m_u.m_rxTeardown + 1u); }
			if constexpr (Tx) { m_u.m_txTeardown = static_cast<uint8_t>(m_u.m_txTeardown + 1u); }
		}
		~TeardownScope()
		{
			if constexpr (Rx) { m_u.m_rxTeardown = static_cast<uint8_t>(m_u.m_rxTeardown - 1u); }
			if constexpr (Tx) { m_u.m_txTeardown = static_cast<uint8_t>(m_u.m_txTeardown - 1u); }
		}
		TeardownScope(const TeardownScope&) = delete;
		TeardownScope& operator=(const TeardownScope&) = delete;
	private:
		Uart& m_u;
	};
	using RxTeardown  = TeardownScope<true, false>;
	using TxTeardown  = TeardownScope<false, true>;
	using AllTeardown = TeardownScope<true, true>;

	/* ---------------------------- ISR: RX ------------------------------- */

	// HAL_UARTEx_RxEventCallback path. `size` is the write position inside the
	// current DMA buffer; since every reception starts on a fresh chunk, it is
	// exactly the number of valid bytes in that chunk.
	void isrRxEvent(const uint16_t size) noexcept
	{
		UART_ENGINE_PROBE_SCOPE(Rx);
		// A stray event delivered while a recovery is pending (m_started
		// cleared, hardware not stopped yet), or one raised by a HAL abort
		// in progress, must not publish anything or re-arm.
		if (!m_started || m_rxTeardown) {
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

	// The byte stream is broken at this point in the sequence.
	void reportGap() noexcept
	{
		if (m_gapHandler) {
			m_gapHandler();
		}
	}

	// Hand a pending overflow discontinuity to the application exactly once,
	// which also releases the producer from the drop buffer.
	void announceGap() noexcept
	{
		if (!m_rxGapPending) {
			return;
		}
		m_rxGapPending = false;
		reportGap();
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
			uart::detail::invalidate_dcache(m_active->data(), size);
			m_active->commit_size(size);
			m_rx.publish();
			m_active = nullptr;
			// Doorbell LAST, so it means "work has been published", not
			// "work is about to be" — matching voidActiveChunk().
			m_rxWorkPending = true;
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
		// While an overflow gap is pending the producer deliberately STAYS in
		// the drop buffer. That is what makes the notification exactly
		// ordered: no chunk received after the loss can enter the queue
		// before proceed() has announced it, so everything the consumer finds
		// queued is guaranteed to predate the gap. Counting chunks could not
		// give this — the ISR keeps publishing while a slow handler runs.
		if (m_active == nullptr && !m_rxGapPending) {
			// Claimed into a local first: reading the value OF an assignment
			// to a volatile operand is deprecated in C++20 (an error in later
			// drafts), and this way the store is the only volatile access.
			RxChunk* const claimed = m_rx.try_claim();
			m_active = claimed;
			if (claimed == nullptr) {
				++m_stats.rx_overrun;
				m_rxGapPending = true; // bytes will be lost; tell the decoder
				m_rxWorkPending = true;
			}
		}
		uint8_t* const dst = (m_active != nullptr) ? m_active->data() : m_drop.data();

		// AN4839: before DMA writes into a cacheable buffer, dirty lines
		// covering it must be discarded, otherwise a later eviction would
		// overwrite freshly received bytes mid-transfer.
		uart::detail::invalidate_dcache(dst, ChunkSize);

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
		{
			RxTeardown ts(*this);
			if (HAL_UART_AbortReceive(m_huart) != HAL_OK) {
				++m_stats.rx_errors;
				m_started = false;
				return;
			}
		}

		uart::detail::IrqGuard guard;

		// Clear stale PE/FE/NE/ORE/IDLE(/RTO) exactly per reference manual:
		// legacy IP -> "read SR then read DR" sequence, new IP -> ICR write.
		// (On legacy IP the DR read may consume one in-flight byte; the
		// restart path is already lossy and COBS resynchronizes on 0x00.)
		uart::detail::clear_rx_errors(m_huart);
		m_huart->ErrorCode = uart::detail::no_error;

		// A chunk still claimed here (e.g. the ISR froze ownership because
		// reception had not stopped) is now safe to touch: the abort above
		// ended the transfer. Its content is unreliable and the line was not
		// being received during the restart, so publish it EMPTY — that
		// zero-length chunk is the in-band discontinuity marker the drain
		// loop turns into a gap notification, in the right place in the
		// stream. Publishing also returns the slot through the normal cycle.
		voidActiveChunk();

		++m_stats.restarts;
		receiveArm();
		__DSB(); __ISB();
	}

	/* ---------------------------- ISR: TX ------------------------------- */

	void isrTxCplt() noexcept
	{
		// Ignore a completion we are not expecting: either nothing is in
		// flight, or this callback was raised by a HAL abort we are running
		// (see m_teardown) — reporting success there would tell the layer
		// above that a frame it never sent has been delivered.
		if (!m_txBusy || m_txTeardown) {
			return;
		}

		// Report the real outcome, not blind success (portable: gState and the
		// TX DMA error code exist on every series with DMA support).
		bool ok = true;
		// gState is read DIRECTLY: HAL_UART_GetState() returns gState|RxState,
		// so while reception is armed (RxState = BUSY_RX = 0x22) an error
		// state (0xE0) reads back as 0xE2 and compares equal to NOTHING —
		// every state test through that accessor is blind in this driver.
		const uint32_t gState = m_huart->gState;
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
	//  - RX errors: with DMA reception active EVERY error is blocking — the
	//    HAL treats "any error occurs in DMA mode reception" that way, so it
	//    runs UART_EndRxTransfer() and aborts the RX DMA before calling us,
	//    leaving RxState READY. That suits us: the partly filled chunk is
	//    voided, which publishes the zero-length marker the drain loop turns
	//    into a GapHandler call, and the decoder resynchronizes instead of
	//    splicing bytes across the corruption;
	//  - TX-side error: HAL ended the transmission -> gState READY.
	// This ISR therefore only classifies, releases ownership and defers the
	// actual re-arm to proceed() (m_started = false), where HAL timeouts work.
	void isrError() noexcept
	{
		// Suppressed only while BOTH directions are being torn down, i.e. when
		// we are deliberately killing the whole peripheral and own its
		// recovery. A single-direction teardown must not hide the other
		// direction's genuine error: the blocking HAL aborts clear
		// XferAbortCallback and do not raise HAL_UART_ErrorCallback
		// themselves, so anything arriving here during one is real.
		const bool rx_teardown = (m_rxTeardown != 0u);
		const bool tx_teardown = (m_txTeardown != 0u);
		if (rx_teardown && tx_teardown) {
			return;
		}
		// Each direction has exactly ONE arbiter of its ownership. Passing the
		// early return is not enough: with a TX teardown in progress the HAL
		// abort has already driven gState to READY, so the TX branch below
		// would release the frame and report failure — stealing the terminal
		// event from the thread-context path that is tearing it down, which
		// reports it too. Mirrored for RX.

		const uint32_t errorCode = HAL_UART_GetError(m_huart);

		if constexpr (uart::detail::new_usart_ip) {
			// ICR write-1-to-clear: side-effect free, always safe.
			uart::detail::clear_rx_errors(m_huart);
		} else if (m_huart->RxState == HAL_UART_STATE_READY) {
			// Legacy IP clears via "read SR then read DR" — the DR read would
			// STEAL a live data byte from the RX DMA stream, so clear only when
			// the reception is already dead. (While it runs, HAL's own IRQ
			// handler has performed the clearing sequence before calling us.)
			uart::detail::clear_rx_errors(m_huart);
		}
		m_huart->ErrorCode = uart::detail::no_error;

		// Direct field read, not HAL_UART_GetState() — see isrTxCplt().
		//
		// NOTE: F1, G4 and H7RS never assign HAL_UART_STATE_ERROR or
		// _TIMEOUT to gState at all — those legacy enum values are unused by
		// the modern UART HAL, which reports faults through ErrorCode. The
		// deadState test is therefore defensive (a family that does use them
		// still gets handled); no recovery logic may depend on it firing.
		const uint32_t gState = m_huart->gState;
		const bool deadState = (gState == HAL_UART_STATE_ERROR) ||
		                       (gState == HAL_UART_STATE_TIMEOUT);

		// RX ownership is decided on RX EVIDENCE ONLY. gState describes the
		// global/TX side of the peripheral, so admitting it here would let a
		// TX-side fault release a chunk that the RX DMA is still writing —
		// the same ownership hazard, wearing a different hat. RxState READY
		// means HAL really did end the reception (it does so even for a
		// TX-triggered DMA fault: UART_DMAError() ends each direction
		// independently), so the chunk holds an unreliable prefix: void it
		// and let proceed() re-arm from thread context.
		if (!rx_teardown && m_huart->RxState == HAL_UART_STATE_READY) {
			++m_stats.rx_errors;
			voidActiveChunk();
			m_started = false; // proceed() -> receiveRestart() with live tick
		} else if (!rx_teardown && (errorCode & uart::detail::rx_error_mask)) {
			// Unreachable while RX runs on DMA (see above); kept for a HAL
			// that would report a line error without ending the reception.
			++m_stats.rx_errors;
		}

		// TX: HAL ended the transmission on a TX-side error -> release the
		// borrowed frame so the layer above can free or retry it.
		if (!tx_teardown && m_txBusy &&
				(deadState || gState == HAL_UART_STATE_READY)) {
			m_txBusy = false;
			++m_stats.tx_errors;
			if (m_txHandler) {
				m_txHandler(false);
			}
		}

		if (m_errHandler && errorCode != uart::detail::no_error) {
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
			m_rxWorkPending = true; // the marker itself is work for the drain
		}
	}

	/* ----------------------- watchdog (main loop) ----------------------- */

	// TX liveness, judged by what the hardware is doing rather than by a
	// per-frame deadline — and resolved TX-ONLY, so a transmitter problem
	// never tears the receiver down and punches a hole in the RX stream.
	// Runs once per audit period from healthCheck().
	void txLivenessAudit() noexcept
	{
		if (!m_txBusy || !m_huart->hdmatx) {
			return;
		}
		const uint32_t remaining = __HAL_DMA_GET_COUNTER(m_huart->hdmatx);

		// A COMPLETION THAT GOT LOST — and note this is a SUCCESS, not a
		// failure. TX ends in two stages: the DMA drains into the peripheral
		// (the HAL only clears DMAT and enables TCIE there) and the shift
		// register empties afterwards, which raises TxCpltCallback. So an
		// empty counter alone proves nothing, but an empty counter TOGETHER
		// with TC means the last stop bit is already on the wire. Every
		// audited family clears TC when a new transfer starts, so it cannot
		// be a leftover from the previous frame. The frame was delivered;
		// only the software event went missing. Stays armed under CTS.
		if (remaining == 0u &&
				__HAL_UART_GET_FLAG(m_huart, UART_FLAG_TC) != 0u &&
				HAL_DMA_GetError(m_huart->hdmatx) == HAL_DMA_ERROR_NONE) {
			finishTx(true);
			return;
		}

		// DMA FINISHED FEEDING THE UART, which is still shifting the tail out
		// of its FIFO and shift register. The counter sits at zero and cannot
		// move again, so "frozen" here means progress-complete, not stalled —
		// the rule above owns what happens next, when TC arrives. Without
		// this the stall detector would report failure for a frame that is
		// physically still going out, which at low baud rates (a full FIFO
		// takes ~0.5 s to drain at 300 baud) is reachable rather than
		// theoretical.
		if (remaining == 0u) {
			return;
		}

		// A TRANSFER THAT STOPPED MOVING. Only a non-zero counter can
		// meaningfully stall. Disabled under CTS, where the peer
		// may legitimately hold the DMA still for as long as it likes. Its
		// own counter is the whole debounce: feeding a confirmed stall into
		// m_failCounter as well would double the latency and drag the
		// receiver into a fault that is not its own.
		if (!m_txProgressWatchEnabled) {
			return;
		}
		if (!m_txProgressValid || remaining != m_txLastRemaining) {
			m_txLastRemaining = remaining;
			m_txProgressValid = true;
			m_txStallChecks = 0u;
			return;
		}
		if (++m_txStallChecks >= UART_ENGINE_FAIL_THRESHOLD) {
			m_txStallChecks = 0u;
			++m_stats.tx_errors;
			finishTx(false);
		}
	}

	// Bring the TX side back to rest and hand the frame's fate to its owner.
	// The abort result is honoured: on HAL_TIMEOUT the DMA may still be
	// reading the caller's buffer, so ownership is NOT returned and the next
	// audit tries again. Whoever claims the terminal event first wins — if the
	// real ISR got there before the gate went up, m_txBusy is already clear
	// and this does nothing.
	void finishTx(const bool ok) noexcept
	{
		bool notify = false;
		{
			TxTeardown ts(*this);
			if (HAL_UART_AbortTransmit(m_huart) == HAL_OK) {
				uart::detail::IrqGuard guard;
				if (m_txBusy) {
					m_txBusy = false;
					m_txProgressValid = false;
					notify = true;
				}
			}
		}
		// Outside both the guard and the gate: the handler may legitimately
		// start the next frame straight away.
		if (notify && m_txHandler) {
			m_txHandler(ok);
		}
	}

	// Periodic self-care: detects silently dead receivers, locked DMA streams
	// and stalled or lost TX transfers on any STM32 series, and recovers
	// without user involvement. Called from proceed() once per
	// UART_ENGINE_CHECK_PERIOD_MS — never on the hot path, so it may take its
	// time. Thread context only.
	void healthCheck() noexcept
	{
		txLivenessAudit();

		// Direct field read, not HAL_UART_GetState() — see isrTxCplt().
		const uint32_t gState = m_huart->gState;
		const uint32_t errorCode = HAL_UART_GetError(m_huart);

		// The gState terms are defensive only (see isrError): on every
		// audited family the effective triggers are ErrorCode, a silently
		// stopped receiver, and the DMA error codes below.
		bool bad = (gState == HAL_UART_STATE_ERROR) ||
		           (gState == HAL_UART_STATE_TIMEOUT) ||
		           (errorCode != uart::detail::no_error);

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
			// Gate FIRST, snapshot SECOND: between reading m_txBusy and
			// raising the gate a genuine TX-complete ISR can still run and
			// report success — and this path would then report failure for
			// the very same frame, giving its owner two lifetime events for
			// one buffer. With the gate up the ISR is silent, so whatever
			// m_txBusy says afterwards is ours alone to act on.
			bool tx_was_active = false;
			{
				AllTeardown ts(*this);
				tx_was_active = m_txBusy;

				(void)HAL_UART_DMAStop(m_huart);
				if (HAL_UART_Abort(m_huart) != HAL_OK) {
					++m_stats.rx_errors;
					m_started = false; // retry from proceed() every iteration
					return;
				}
				uart::detail::IrqGuard guard;
				voidActiveChunk(); // transfers confirmed stopped
				m_started = false; // RX hardware is down; keep software in step
				m_txBusy = false;
			}
			// The in-flight frame died with the peripheral: tell the owner,
			// outside the gate, so it can release or retry it.
			if (tx_was_active) {
				++m_stats.tx_errors;
				if (m_txHandler) {
					m_txHandler(false);
				}
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
	// The POINTER is volatile (the pointee is not): an ISR re-arms onto a new
	// chunk while thread code is inside the unguarded HAL abort of the
	// drop-reclaim path, and that code must then observe the new value. A
	// plain member would let the compiler keep the stale nullptr it just
	// tested — an ISR write is not a visible side effect in the C++ memory
	// model, so nothing but volatile (or an atomic) obliges it to reload.
	RxChunk* volatile m_active = nullptr; // chunk owned by DMA (claimed, unpublished)

	// Fallback DMA target when all chunks are in flight; contents are discarded.
	alignas(32) std::array<uint8_t, ChunkSize> m_drop{};

	UART_HandleTypeDef* m_huart = nullptr;
	volatile bool m_started = false;
	volatile bool m_txBusy  = false;

	RxHandler    m_rxHandler{};
	TxHandler    m_txHandler{};
	ErrorHandler m_errHandler{};
	GapHandler   m_gapHandler{};

	// Set by the ISR when the pool ran dry, cleared by the drain loop.
	volatile bool m_rxGapPending = false;
	// Doorbell: raised by the ISR whenever it leaves work for the consumer,
	// so the hot path can answer "is there anything to do?" without touching
	// the SPSC queue and paying its acquire barrier on every iteration.
	volatile bool m_rxWorkPending = false;
	// TX liveness observation (thread context only). Stall detection is off
	// while CTS flow control may legitimately hold the transmitter still.
	bool     m_txProgressWatchEnabled = false;
	bool     m_txProgressValid = false;
	uint32_t m_txLastRemaining = 0;
	uint8_t  m_txStallChecks = 0;

	// Non-zero while thread code is inside a HAL teardown. ST documents that
	// HAL_DMA_Abort() — reached through every HAL_UART_Abort*/DMAStop call —
	// raises the TX/RX (half-)complete interrupt for a transfer it interrupts
	// mid-stream, so the corresponding callback runs. Without this gate such a
	// callback is indistinguishable from a real completion: TX would report
	// success for a frame that was never sent, and RX would publish a partial
	// chunk and re-arm DMA in the middle of the teardown. A depth counter,
	// not a flag: recovery nests (healthCheck -> receiveRestart).
	// One counter PER DIRECTION: RX and TX tear down independently, and a
	// single gate would let an RX abort swallow a genuine TX completion (and
	// vice versa) for the microseconds it runs. Full recovery raises both.
	volatile uint8_t m_rxTeardown = 0;
	volatile uint8_t m_txTeardown = 0;

	// Watchdog state (thread context only).
	uint32_t m_lastCheckTime = 0;
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
	uart::detail::Registry::onRxEvent(huart, Size);
}

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* huart)
{
	uart::detail::Registry::onTxCplt(huart);
}

extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart)
{
	uart::detail::Registry::onError(huart);
}

#endif /* UART_ENGINE_IMPLEMENT */
#endif /* internal callbacks */

#endif /* HAL_UART_MODULE_ENABLED */
#endif /* UART_ENGINE_UART_H_ */
