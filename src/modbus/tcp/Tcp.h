/* Author: shpegun60; SPDX-License-Identifier: MIT */
#ifndef MODBUS_TCP_H_
#define MODBUS_TCP_H_
#include "../Pdu.h"
#include "Format.h"
#include "Stats.h"
#include "detail/Message.h"
#include "detail/Receiver.h"
#include "tiny_delegate.hpp"
#include <concepts>
#include <type_traits>
#include <utility>

namespace modbus::tcp {
using SendResult = modbus::SendResult;
using modbus::read_native;
using modbus::read_be;
using modbus::read_le;
using modbus::read_bytes;

// No socket, HAL, timer, transaction scheduler or user-supplied framer.
// External serialization and lifetime/borrow rules match RTU and COBS.
template<class MemoryT = wire::Heap, class FormatT = modbus::tcp::Format<>>
class Endpoint final {
public:
	using SendResult = wire::SendResult;
	using Memory = MemoryT;
	using Format = FormatT;
	using Crc = typename Format::Crc;
	using Layout = typename Format::Layout;
	using Geometry = detail::GeometryFor<Layout>;
	static_assert(wire::Storage<MemoryT, Geometry>,
		"Endpoint storage must satisfy the wire::Storage contract");
	using Storage = typename MemoryT::template For<Geometry>;
	using Message = modbus::tcp::Message<Storage, Layout>;
	using Packet = modbus::tcp::Packet<Storage, Layout>;
	static constexpr bool framed = true;
	static constexpr std::size_t crc_size = Layout::crc_size;
	static constexpr std::size_t max_receive_size = Layout::max_data_size;
	static constexpr std::size_t max_send_size = Layout::max_data_size;
	static constexpr std::size_t max_frame_size = Layout::max_adu_size;
	static constexpr std::size_t default_capacity_hint = max_send_size < 32u ? max_send_size : 32u;
	using Sender = tiny::delegate<bool(std::span<const uint8_t>)>;
	using BusyQuery = tiny::delegate<bool()>;

	Endpoint() noexcept(std::is_nothrow_default_constructible_v<Storage> && std::is_nothrow_default_constructible_v<Crc>)
		requires std::default_initializable<Storage> && std::default_initializable<Crc> = default;
	explicit Endpoint(Crc crc) noexcept(std::is_nothrow_default_constructible_v<Storage> && std::is_nothrow_move_constructible_v<Crc>)
		requires std::default_initializable<Storage> && std::constructible_from<Crc, Crc&&>
		: m_crc(std::move(crc)) {}
	template<class... Args>
	explicit Endpoint(std::in_place_t, Args&&... args) noexcept(
		std::is_nothrow_constructible_v<Storage, Args&&...> && std::is_nothrow_default_constructible_v<Crc>)
		requires std::constructible_from<Storage, Args&&...> && std::default_initializable<Crc>
		: m_storage(std::forward<Args>(args)...) {}
	template<class... Args>
	Endpoint(Crc crc, std::in_place_t, Args&&... args) noexcept(
		std::is_nothrow_constructible_v<Storage, Args&&...> && std::is_nothrow_move_constructible_v<Crc>)
		requires std::constructible_from<Storage, Args&&...> && std::constructible_from<Crc, Crc&&>
		: m_storage(std::forward<Args>(args)...), m_crc(std::move(crc)) {}
	Endpoint(const Endpoint&) = delete;
	Endpoint& operator=(const Endpoint&) = delete;
	Endpoint(Endpoint&&) = delete;
	Endpoint& operator=(Endpoint&&) = delete;
	~Endpoint()
	{
		// The application MUST stop the transport before destroying its owner.
		if (m_active_tx.memory != nullptr) { m_storage.release_tx(m_active_tx); }
	}
	[[nodiscard]] bool bind(Sender sender, BusyQuery busy) noexcept
	{
		if (tx_active() || !sender || !busy) { return false; }
		m_sender = std::move(sender);
		m_busy = std::move(busy);
		return true;
	}
	[[nodiscard]] bool unbind() noexcept
	{
		if (tx_active()) { return false; }
		m_sender = nullptr;
		m_busy = nullptr;
		return true;
	}
	void consume(const std::span<const uint8_t> bytes) noexcept { m_receiver.consume(m_crc, bytes); }
	void notify_gap() noexcept { m_receiver.notify_gap(); }
	void reset_rx() noexcept { m_receiver.reset_rx(); }
	[[nodiscard]] bool rx_failed() const noexcept { return m_receiver.rx_failed(); }
	[[nodiscard]] bool assembling() const noexcept { return m_receiver.assembling(); }
	[[nodiscard]] bool has_packet() const noexcept { return m_receiver.has_packet(); }
	[[nodiscard]] Packet pop_packet() noexcept { return m_receiver.pop_packet(); }
	[[nodiscard]] Message make_message(const uint16_t transaction, const uint8_t unit,
		const uint8_t function, const std::size_t capacity_hint = default_capacity_hint) noexcept
	{
		return Message{m_storage, transaction, unit, function, capacity_hint};
	}
	[[nodiscard]] SendResult send(Message& message) noexcept
	{
		if (!message || !message.belongs_to(m_storage)) { return SendResult::Invalid; }
		if (!m_sender || !m_busy) { return SendResult::Unbound; }
		if (tx_active() || m_busy()) { ++m_tx_stats.send_refused_busy; return SendResult::Busy; }
		const auto adu = message.finalize(m_crc);
		if (!m_sender(adu)) { ++m_tx_stats.send_failed; return SendResult::Failed; }
		m_active_tx = message.surrender_block();
		++m_tx_stats.frames_sent;
		return SendResult::Sent;
	}
	[[nodiscard]] bool tx_active() const noexcept { return m_active_tx.memory != nullptr; }
	void poll(const uint32_t now_ms) noexcept
	{
		(void)now_ms;
		if (tx_active() && !m_busy()) {
			m_storage.release_tx(m_active_tx);
			m_active_tx = {};
		}
	}
	[[nodiscard]] Stats stats() const noexcept { return {m_receiver.stats(), m_tx_stats}; }
	[[nodiscard]] const Storage& storage() const noexcept { return m_storage; }
private:
	using Block = detail::RxBlock<Storage>;
	using Shape = detail::RxBlock<detail::AnyStorage>;
	static_assert(sizeof(Block) == sizeof(Shape) && alignof(Block) == alignof(Shape));
	[[no_unique_address]] Storage m_storage{};
	detail::Receiver<Storage, Layout> m_receiver{m_storage};
	Sender m_sender{};
	BusyQuery m_busy{};
	wire::TxBlock m_active_tx{};
	Stats::Tx m_tx_stats{};
	[[no_unique_address]] Crc m_crc{};
};
} // namespace modbus::tcp
#endif
