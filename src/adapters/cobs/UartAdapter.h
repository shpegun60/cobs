/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef COBS_UART_ADAPTER_H_
#define COBS_UART_ADAPTER_H_

#include "tiny_delegate.hpp"

#include <concepts>
#include <cstdint>
#include <span>

namespace cobs {

/*
 * The STM32 UART composition, with the same application-facing lifecycle as
 * modbus::rtu::UartAdapter: construct(serial, endpoint), initialize the driver,
 * bind(), then proceed(now_ms) from one loop/task. No clock, CRC, framing table
 * or DMA ownership is added here: consume() already understands every cut of
 * a COBS stream. There is no incomplete-frame deadline; a delimiter restores
 * synchronization. The core and the driver do not depend on this adapter.
 *
 * Driver and endpoint must outlive a bound adapter. One adapter per driver;
 * unbind the previous adapter before installing another. bind()/unbind() are
 * transactional and refuse a TX block still held by the endpoint, even if
 * the driver is already idle (proceed() reclaims it). The destructor detaches
 * RX/gap handlers; TX delegates target the driver, not this adapter, and stay
 * valid until the endpoint releases its borrow. Drain TX before destroying
 * the driver or endpoint.
 *
 * Detaching is a stream discontinuity, so it calls notify_gap(): a partial RX
 * block is returned, already queued packets survive, and bytes through the
 * next delimiter are discarded. This is counted by the normal COBS gap
 * counters. A rebind cannot complete a frame from the previous binding; the
 * first new frame may supply that synchronization delimiter and be discarded.
 */
template<class SerialT, class EndpointT>
class UartAdapter final {
	static_assert(requires(EndpointT& endpoint, std::span<const uint8_t> bytes, uint32_t now) {
		{ endpoint.consume(bytes) } noexcept -> std::same_as<void>;
		{ endpoint.notify_gap() } noexcept -> std::same_as<void>;
		{ endpoint.poll(now) } noexcept -> std::same_as<void>;
	} && !requires { EndpointT::framed; },
		"UartAdapter serves a cobs::Endpoint; RTU needs its own stale-frame adapter");

public:
	using Serial = SerialT;
	using Endpoint = EndpointT;
	static constexpr uint32_t no_deadline = UINT32_MAX;

	UartAdapter(SerialT& uart, EndpointT& endpoint) noexcept
		: m_uart(uart), m_endpoint(endpoint) {}

	UartAdapter(const UartAdapter&) = delete;
	UartAdapter& operator=(const UartAdapter&) = delete;

	~UartAdapter()
	{
		if (m_bound) {
			detach();
		}
	}

	[[nodiscard]] bool bound() const noexcept { return m_bound; }

	[[nodiscard]] bool bind() noexcept
	{
		const auto* const handle = m_uart.instance();
		if (handle == nullptr || handle->Init.BaudRate == 0u) {
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

	void proceed(const uint32_t now_ms) noexcept
	{
		m_uart.proceed(now_ms);
		m_endpoint.poll(now_ms);
	}

	void on_rx(const std::span<const uint8_t> bytes) noexcept { m_endpoint.consume(bytes); }
	void on_gap() noexcept { m_endpoint.notify_gap(); }

	// Same scheduler vocabulary as the RTU adapter, without timer state.
	[[nodiscard]] constexpr uint32_t deadline_in_ms(uint32_t) const noexcept { return no_deadline; }
	[[nodiscard]] constexpr bool deadline_armed() const noexcept { return false; }

private:
	void detach() noexcept
	{
		m_uart.setRxHandler(typename SerialT::RxHandler{});
		m_uart.setRxGapHandler(typename SerialT::GapHandler{});
		m_endpoint.notify_gap();
		m_bound = false;
	}

	SerialT& m_uart;
	EndpointT& m_endpoint;
	bool m_bound = false;
};

} // namespace cobs

#endif /* COBS_UART_ADAPTER_H_ */
