/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * modbus::rtu::UartAdapter — the whole integration between the STM32 UART
 * driver (src/uart/Uart.h) and an RTU endpoint, in one object the application
 * services with one call. It lives in src/adapters because it knows both
 * sides; neither the driver nor the protocol knows it exists.
 *
 *     using Serial = Uart<256, 4>;
 *     using Link   = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
 *                        modbus::rtu::framing::Standard<framing::Direction::Request>>;
 *
 *     static Serial serial;
 *     static Link link;
 *     static modbus::rtu::UartAdapter adapter{serial, link};   // safe at static-init time
 *
 *     serial.init(&huart3);      // first: the adapter reads the line rate from the bound handle
 *     adapter.bind();
 *
 *     // main loop, or one communication task
 *     adapter.proceed(HAL_GetTick());
 *     while (auto packet = link.pop_packet()) { handle(packet); }
 *
 * The adapter owns everything that used to be application glue: the RX and
 * gap handler binding, the endpoint's transport binding, the order of the
 * slow-path calls, and — for an endpoint with a framing policy — the rule
 * that decides when a frame that stopped arriving is dead. That rule needs
 * two facts only this layer has: the driver's chunk capacity, read from the
 * Uart<ChunkSize, ChunkCount> type through UartTraits, and how each chunk
 * ended, read from its size. The endpoint itself keeps no clock: it exposes
 * assembling() and expire_incomplete(), and stays transport-agnostic.
 *
 * Lifetime and binding. The driver and the endpoint must outlive the adapter
 * while it is bound; the destructor detaches the driver's RX and gap handlers
 * so a destroyed adapter can never be called. bind() is transactional: the
 * endpoint's transport binding is attempted first and, only when it succeeded,
 * the driver's handlers are pointed at the adapter (the driver's setters
 * cannot fail), so a false return leaves both objects exactly as they were.
 * bind() requires an initialized driver, because the line rate is read from
 * the bound HAL handle — the single source of truth; the adapter keeps no
 * configuration of its own and re-reads that rate on every proceed(), so
 * Uart::setBaudRate() is followed without a second call. One adapter serves
 * one driver at a time: binding a second adapter over a bound one replaces
 * the driver's handlers (the endpoint permits rebinding while no transmission
 * is active), and the first adapter's unbind() or destructor would then
 * detach the second's handlers — unbind the first before binding another.
 *
 * The stale-frame rule. The driver publishes a chunk either because the
 * line went idle (a PARTIAL chunk, size < ChunkSize) or because the chunk
 * filled (a FULL chunk, size == ChunkSize; an idle line exactly on the
 * boundary looks the same and is treated the same, conservatively).
 *
 *   - A frame still incomplete after a partial chunk means the line fell
 *     silent mid-frame. A serial bridge that split the frame resumes within
 *     microseconds, so idle_stale_ms (5 ms) of silence is a dead frame. The
 *     clock starts at the IDLE event, which itself follows about one
 *     character of silence, so on any usual Modbus baud the total is far
 *     beyond the t1.5 limit inside a frame; it is a pragmatic guard, not a
 *     derivation from t1.5 for every conceivable baud. Silence is judged by
 *     the hardware, not by the absence of events: when the 5 ms fall due
 *     the adapter asks the driver (Uart::rx_progress()) whether DMA has
 *     already taken bytes into the chunk it still owns. It has whenever the
 *     bridge resumed and the remainder is arriving but has not yet ended in
 *     IDLE or filled the chunk, a state that lasts the remainder's whole
 *     transfer time (14 ms for 150 bytes at 115200, far longer at 9600);
 *     then the frame is alive and the deadline becomes one chunk time plus
 *     the guard, as after a full chunk. Zero progress after 5 ms is a dead
 *     frame.
 *   - After a full chunk the line is still busy: the next chunk needs
 *     ChunkSize character times to arrive, and only silence longer than that
 *     plus a guard is a dead frame. This is what a sender that dies exactly on
 *     a chunk boundary is caught by, and what keeps a 1024-byte private ADU
 *     alive across four 256-byte chunks at 9600 baud (about 320 ms each).
 *     A character is taken as 12 bits — start, up to nine data/parity bits
 *     (the driver accepts 8N and 9B-with-parity) and up to two stop bits —
 *     so the deadline is never shorter than the wire, and on 8N1 a fifth
 *     longer than necessary. The full-chunk deadline assumes a continuously
 *     transmitting peer or bridge whose stalls inside a frame are small
 *     against a chunk's transmission; strict RTU permits pauses below t1.5
 *     between EVERY character, and a peer that used that allowance on every
 *     byte could stretch a chunk beyond this deadline. This adapter targets
 *     DMA peers and USB bridges, which do not; it is not a t1.5 timer.
 *
 * No time is recorded in the receive path. on_rx() stamps a deadline with
 * the tick proceed() was given; the progress snapshot is taken in proceed()
 * BEFORE the driver is drained and the verdict AFTER, so a continuation that
 * already sits in the driver's queue, or is moved there while it drains, is
 * never outrun by its own deadline. A loop that must keep its own timing
 * scopes around the driver composes the same steps with prepare(now) →
 * serial.proceed(now) → finish(now) → endpoint.poll(now); that order is part
 * of the contract. unbind() and the destructor discard a frame in flight,
 * uncounted: what arrives after a later bind() can never complete it.
 *
 * A task that sleeps between calls must not sleep past the deadline:
 * deadline_in_ms(now) bounds the wait (no_deadline when nothing is in
 * flight, 0 when due), see adapters/freertos/FreeRtosWake.h.
 *
 * The adapter does not include Uart.h: it needs only the driver's type
 * shape and the members every driver instantiation has (setRxHandler,
 * setRxGapHandler, send, tx_busy, proceed, instance, rx_progress), so it
 * compiles against the host fake HAL in the test suite exactly as against
 * the silicon driver.
 */

#ifndef MODBUS_RTU_UART_ADAPTER_H_
#define MODBUS_RTU_UART_ADAPTER_H_

#include "tiny_delegate.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

// uart/Uart.h, declared, not included: see the header comment.
template<std::size_t ChunkSize, std::size_t ChunkCount>
class Uart;

namespace modbus::rtu {

// The chunk geometry of a UART driver type, deduced from the type rather than
// asked of the (frozen) driver.
template<class Serial>
struct UartTraits;

template<std::size_t ChunkSize, std::size_t ChunkCount>
struct UartTraits<::Uart<ChunkSize, ChunkCount>> {
	static constexpr std::size_t chunk_size = ChunkSize;
	static constexpr std::size_t chunk_count = ChunkCount;
};

// What the adapter needs from its endpoint: the RTU endpoint's shape.
template<class E>
concept RtuEndpoint = requires(E& endpoint, uint32_t now_ms) {
	{ E::framed } -> std::convertible_to<bool>;
	endpoint.notify_gap();
	endpoint.poll(now_ms);
};

// What the adapter needs from the driver beyond its handler setters and the
// transport pair: the bound handle and the DMA progress snapshot.
template<class S>
concept ProgressReportingSerial = requires(const S& serial) {
	{ serial.rx_progress() } -> std::convertible_to<uint16_t>;
	serial.instance();
};

template<class SerialT, class EndpointT>
class UartAdapter final {
	static_assert(RtuEndpoint<EndpointT>, "UartAdapter serves a modbus::rtu::Endpoint");
	static_assert(ProgressReportingSerial<SerialT>,
		"UartAdapter needs Uart::instance() and Uart::rx_progress()");

public:
	using Serial = SerialT;
	using Endpoint = EndpointT;

	static constexpr std::size_t chunk_size = UartTraits<SerialT>::chunk_size;
	static constexpr bool framed = EndpointT::framed;

	// Silence after a partial chunk that ends a frame in flight (see above).
	static constexpr uint32_t idle_stale_ms = 5u;
	// Added to one chunk's transfer time after a full chunk.
	static constexpr uint32_t full_chunk_guard_ms = 5u;
	// The widest character the driver accepts: start + 9 data/parity + 2 stop.
	static constexpr uint32_t bits_per_character = 12u;
	// deadline_in_ms() when no frame is in flight.
	static constexpr uint32_t no_deadline = UINT32_MAX;

	static_assert(static_cast<uint64_t>(chunk_size) * bits_per_character * 1000u <= UINT32_MAX,
		"chunk transfer time arithmetic must fit 32 bits");

	// No configuration is taken here: it is safe to construct at static-init
	// time, before the HAL handle carries any line rate.
	UartAdapter(SerialT& uart, EndpointT& endpoint) noexcept
		: m_uart(uart), m_endpoint(endpoint) {}

	UartAdapter(const UartAdapter&) = delete;
	UartAdapter& operator=(const UartAdapter&) = delete;

	// A bound adapter detaches itself from the driver, which must still be
	// alive (see the lifetime contract above). The endpoint's transport binding
	// is left in place: it points at the driver, not at the adapter.
	~UartAdapter()
	{
		if (m_bound) {
			detach();
		}
	}

	// Milliseconds one full chunk takes on the wire at `baud`, rounded up; 0
	// for a baud of 0.
	[[nodiscard]] static constexpr uint32_t chunk_time_ms(const uint32_t baud) noexcept
	{
		if (baud == 0u) {
			return 0u;
		}
		const uint32_t bit_milliseconds =
			static_cast<uint32_t>(chunk_size) * bits_per_character * 1000u;
		// Rounded up without the `+ baud - 1` that overflows for an absurd baud.
		return bit_milliseconds / baud + ((bit_milliseconds % baud) != 0u ? 1u : 0u);
	}

	[[nodiscard]] uint32_t baud() const noexcept { return m_baud; }
	[[nodiscard]] uint32_t full_chunk_ms() const noexcept { return m_full_chunk_ms; }
	[[nodiscard]] bool bound() const noexcept { return m_bound; }

	/*
	 * Wires the driver to the endpoint. Requires an initialized driver (the
	 * line rate comes from its bound HAL handle). Transactional: false — the
	 * driver is not initialized, its handle carries no rate, or the endpoint
	 * refuses the transport binding because a transmission is still active —
	 * leaves both the driver's handlers and the endpoint untouched.
	 */
	[[nodiscard]] bool bind() noexcept
	{
		if (!refresh_timing()) {
			return false;
		}
		if (!m_endpoint.bind(
				typename EndpointT::Sender{tiny::bind<&SerialT::send>(m_uart)},
				typename EndpointT::BusyQuery{tiny::bind<&SerialT::tx_busy>(m_uart)})) {
			return false;
		}
		m_uart.setRxHandler(typename SerialT::RxHandler{
			tiny::bind<&UartAdapter::on_rx>(*this)});
		m_uart.setRxGapHandler(typename SerialT::GapHandler{
			tiny::bind<&UartAdapter::on_gap>(*this)});
		m_bound = true;
		return true;
	}

	// The reverse, equally transactional: false while a transmission is still
	// borrowed by the driver, and then nothing has changed.
	[[nodiscard]] bool unbind() noexcept
	{
		if (!m_bound) {
			return true;
		}
		if (!m_endpoint.unbind()) {
			return false;
		}
		detach();
		return true;
	}

	/*
	 * The one slow-path call: the progress snapshot is taken, the driver
	 * delivers what it has (on_rx/on_gap run inside), THEN an overdue frame is
	 * judged, then the endpoint reclaims a finished transmission. `now_ms` is
	 * the application's monotonic millisecond tick; on_rx() stamps deadlines
	 * with it.
	 */
	void proceed(const uint32_t now_ms) noexcept
	{
		prepare(now_ms);
		m_uart.proceed(now_ms);
		finish(now_ms);
		m_endpoint.poll(now_ms);
	}

	/*
	 * The two halves of proceed() around the driver, for a loop that keeps its
	 * own timing scopes (the hardware harness); both take the same tick.
	 * prepare() records the tick on_rx() will stamp deadlines with, follows a
	 * changed line rate and, when a frame's deadline is due, asks the driver
	 * whether DMA has taken bytes into the chunk it still owns. finish()
	 * judges the frame: extended if the line resumed, expired if not. The
	 * snapshot BEFORE the driver is drained and the verdict AFTER is what
	 * makes the instant of the deadline safe: a byte that arrived before the
	 * snapshot is seen either as progress or, if an event moved its chunk to
	 * the driver's queue in between, as a delivered chunk that re-stamps the
	 * deadline. Bytes that only begin to arrive after the snapshot are late
	 * by definition. finish() must therefore come AFTER the driver's
	 * proceed(), and prepare() before it.
	 */
	void prepare(const uint32_t now_ms) noexcept
	{
		m_now_ms = now_ms;
		(void)refresh_timing();
		if constexpr (framed) {
			m_line_resumed = m_deadline_active && due(now_ms) &&
				m_uart.rx_progress() != 0u;
		}
	}

	void finish(const uint32_t now_ms) noexcept
	{
		if constexpr (framed) {
			if (m_deadline_active && due(now_ms)) {
				if (m_line_resumed) {
					// The next chunk is filling: it must be published within one
					// chunk's transfer time, exactly as after a full chunk.
					m_deadline_ms = now_ms + m_full_chunk_ms + full_chunk_guard_ms;
				} else {
					m_deadline_active = false;
					m_endpoint.expire_incomplete();
				}
			}
			m_line_resumed = false;
		} else {
			(void)now_ms;
		}
	}

	// Transport entry points. bind() routes the driver here; a harness or a
	// different transport may call them directly.
	void on_rx(const std::span<const uint8_t> bytes) noexcept
	{
		if constexpr (framed) {
			m_endpoint.consume(bytes);
			if (!m_endpoint.assembling()) {
				m_deadline_active = false;
				return;
			}
			m_deadline_ms = m_now_ms + (bytes.size() < chunk_size
				? idle_stale_ms
				: m_full_chunk_ms + full_chunk_guard_ms);
			m_deadline_active = true;
		} else {
			m_endpoint.receive_adu(bytes);
		}
	}

	void on_gap() noexcept
	{
		m_deadline_active = false;
		m_endpoint.notify_gap();
	}

	// Milliseconds until the frame in flight is declared dead: 0 when due,
	// no_deadline when nothing is in flight. A scheduler can sleep this long.
	[[nodiscard]] uint32_t deadline_in_ms(const uint32_t now_ms) const noexcept
	{
		if (!m_deadline_active) {
			return no_deadline;
		}
		const int32_t left = static_cast<int32_t>(m_deadline_ms - now_ms);
		return left <= 0 ? 0u : static_cast<uint32_t>(left);
	}

	[[nodiscard]] bool deadline_armed() const noexcept { return m_deadline_active; }

private:
	// The line rate lives in the driver's HAL handle and may change through
	// Uart::setBaudRate(); one load and one compare per call, the division
	// only when it changed. False when the driver is not initialized or the
	// handle carries no rate.
	[[nodiscard]] bool refresh_timing() noexcept
	{
		const auto* const handle = m_uart.instance();
		if (handle == nullptr || handle->Init.BaudRate == 0u) {
			return false;
		}
		const uint32_t baud = handle->Init.BaudRate;
		if (baud != m_baud) {
			m_baud = baud;
			m_full_chunk_ms = chunk_time_ms(baud);
		}
		return true;
	}

	[[nodiscard]] bool due(const uint32_t now_ms) const noexcept
	{
		return static_cast<int32_t>(now_ms - m_deadline_ms) >= 0;
	}

	// Detaching is a discontinuity of the stream: a frame in flight can never
	// be completed by what arrives after a later bind(), so it is discarded,
	// uncounted (it is neither stale nor lost).
	void detach() noexcept
	{
		m_uart.setRxHandler(typename SerialT::RxHandler{});
		m_uart.setRxGapHandler(typename SerialT::GapHandler{});
		if constexpr (framed) {
			m_endpoint.discard_incomplete();
		}
		m_deadline_active = false;
		m_line_resumed = false;
		m_bound = false;
	}

	SerialT& m_uart;
	EndpointT& m_endpoint;
	uint32_t m_baud = 0u;
	uint32_t m_full_chunk_ms = 0u;
	uint32_t m_now_ms = 0u;
	uint32_t m_deadline_ms = 0u;
	bool m_deadline_active = false;
	bool m_line_resumed = false;
	bool m_bound = false;
};

} // namespace modbus::rtu

#endif /* MODBUS_RTU_UART_ADAPTER_H_ */
