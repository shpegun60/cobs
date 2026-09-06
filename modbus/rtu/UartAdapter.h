/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * modbus::rtu::UartAdapter — the whole integration between the STM32 UART
 * driver (uart/Uart.h) and an RTU endpoint, in one object the application
 * services with one call.
 *
 *     using Serial = Uart<256, 4>;
 *     using Link   = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
 *                        modbus::rtu::framing::Standard<framing::Direction::Request>>;
 *
 *     Serial uart;
 *     Link link;
 *     modbus::rtu::UartAdapter adapter{uart, link, huart3.Init.BaudRate};
 *
 *     uart.init(&huart3);
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
 *     derivation from t1.5 for every conceivable baud.
 *   - After a full chunk the line is still busy: the next chunk needs
 *     ChunkSize character times to arrive, and only silence longer than that
 *     plus a guard is a dead frame. This is what a sender that dies exactly on
 *     a chunk boundary is caught by, and what keeps a 1024-byte private ADU
 *     alive across four 256-byte chunks at 9600 baud (about 300 ms each).
 *     Character time is taken as 11 bits (start, 8 data, parity, stop): on
 *     8N1 links the deadline is a tenth longer than necessary, never shorter.
 *
 * No time is recorded in the receive path. on_rx() stamps a deadline with
 * the tick proceed() was given; the deadline is checked in proceed(), after
 * the driver has delivered what it had.
 *
 * The adapter does not include Uart.h: it needs only the driver's type
 * shape and the four members every driver instantiation has (setRxHandler,
 * setRxGapHandler, send, tx_busy, proceed), so it compiles against the host
 * fake HAL in the test suite exactly as against the silicon driver.
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
concept RtuEndpoint = requires(E& endpoint, std::span<const uint8_t> bytes, uint32_t now_ms) {
	{ E::framed } -> std::convertible_to<bool>;
	endpoint.notify_gap();
	endpoint.poll(now_ms);
};

template<class SerialT, class EndpointT>
class UartAdapter final {
	static_assert(RtuEndpoint<EndpointT>, "UartAdapter serves a modbus::rtu::Endpoint");

public:
	using Serial = SerialT;
	using Endpoint = EndpointT;

	static constexpr std::size_t chunk_size = UartTraits<SerialT>::chunk_size;
	static constexpr bool framed = EndpointT::framed;

	// Silence after a partial chunk that ends a frame in flight (see above).
	static constexpr uint32_t idle_stale_ms = 5u;
	// Added to one chunk's transfer time after a full chunk.
	static constexpr uint32_t full_chunk_guard_ms = 5u;
	// Worst-case wire character: start + 8 data + parity + stop.
	static constexpr uint32_t bits_per_character = 11u;
	// deadline_in_ms() when no frame is in flight.
	static constexpr uint32_t no_deadline = UINT32_MAX;

	// `baud` is the line rate the UART handle was initialized with
	// (huart.Init.BaudRate): the handle is the single source of truth and the
	// driver exposes no copy of it.
	UartAdapter(SerialT& uart, EndpointT& endpoint, const uint32_t baud) noexcept
		: m_uart(uart), m_endpoint(endpoint), m_full_chunk_ms(chunk_time_ms(baud)) {}

	UartAdapter(const UartAdapter&) = delete;
	UartAdapter& operator=(const UartAdapter&) = delete;

	// Milliseconds one full chunk takes on the wire at `baud`, rounded up; 0
	// for a baud of 0, which bind() refuses.
	[[nodiscard]] static constexpr uint32_t chunk_time_ms(const uint32_t baud) noexcept
	{
		if (baud == 0u) {
			return 0u;
		}
		const uint64_t bit_milliseconds =
			static_cast<uint64_t>(chunk_size) * bits_per_character * 1000u;
		return static_cast<uint32_t>((bit_milliseconds + baud - 1u) / baud);
	}

	[[nodiscard]] uint32_t full_chunk_ms() const noexcept { return m_full_chunk_ms; }

	/*
	 * Wires the driver to the endpoint: RX chunks and gaps into on_rx()/on_gap(),
	 * the endpoint's transmit path onto the driver's send()/tx_busy(). Callable
	 * before or after Uart::init(). False when the baud is 0 (no deadline can be
	 * computed) or when the endpoint refuses the transport binding (a
	 * transmission is still active).
	 */
	[[nodiscard]] bool bind() noexcept
	{
		if (m_full_chunk_ms == 0u) {
			return false;
		}
		m_uart.setRxHandler(typename SerialT::RxHandler{
			tiny::bind<&UartAdapter::on_rx>(*this)});
		m_uart.setRxGapHandler(typename SerialT::GapHandler{
			tiny::bind<&UartAdapter::on_gap>(*this)});
		return m_endpoint.bind(
			typename EndpointT::Sender{tiny::bind<&SerialT::send>(m_uart)},
			typename EndpointT::BusyQuery{tiny::bind<&SerialT::tx_busy>(m_uart)});
	}

	/*
	 * The one slow-path call: the driver delivers what it has (on_rx/on_gap run
	 * inside), an overdue frame is expired, the endpoint reclaims a finished
	 * transmission. `now_ms` is the application's monotonic millisecond tick;
	 * on_rx() stamps deadlines with it.
	 */
	void proceed(const uint32_t now_ms) noexcept
	{
		m_now_ms = now_ms;
		m_uart.proceed(now_ms);
		expire_due(now_ms);
		m_endpoint.poll(now_ms);
	}

	// The adapter's own step, for a loop that calls the driver and the endpoint
	// itself (the hardware harness keeps its own timing scopes around them):
	// records the tick on_rx() will stamp deadlines with and expires an overdue
	// frame. Call it before the driver's proceed() in such a loop.
	void service(const uint32_t now_ms) noexcept
	{
		m_now_ms = now_ms;
		expire_due(now_ms);
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
	void expire_due(const uint32_t now_ms) noexcept
	{
		if constexpr (framed) {
			if (m_deadline_active &&
			    static_cast<int32_t>(now_ms - m_deadline_ms) >= 0) {
				m_deadline_active = false;
				m_endpoint.expire_incomplete();
			}
		} else {
			(void)now_ms;
		}
	}

	SerialT& m_uart;
	EndpointT& m_endpoint;
	const uint32_t m_full_chunk_ms;
	uint32_t m_now_ms = 0u;
	uint32_t m_deadline_ms = 0u;
	bool m_deadline_active = false;
};

} // namespace modbus::rtu

#endif /* MODBUS_RTU_UART_ADAPTER_H_ */
