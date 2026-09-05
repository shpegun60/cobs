/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Modbus RTU endpoint.
 *
 * Ownership and transport semantics deliberately mirror cobs::Endpoint.
 * The RX boundary is different: receive_adu() accepts exactly one physical
 * UART burst candidate, not arbitrary stream chunks. The candidate is
 * validated before one copy into Packet-owned storage.
 */

#ifndef MODBUS_RTU_H_
#define MODBUS_RTU_H_

#include "../Pdu.h"
#include "Crc.h"
#include "Format.h"
#include "Stats.h"
#include "Storage.h"
#include "detail/Message.h"
#include "detail/Packet.h"
#include "detail/Receiver.h"

#include "tiny_delegate.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace modbus::rtu {

using SendResult = modbus::SendResult;

template<class StorageT = Heap, class CrcT = modbus::rtu::crc::Bitwise>
class Endpoint final {
	static_assert(modbus::rtu::Storage<StorageT>,
		"Endpoint storage must satisfy modbus::rtu::Storage");
	static_assert(::crc::Policy<CrcT>,
		"Endpoint CRC must satisfy crc::Policy");
	static_assert(StorageT::max_adu_size == modbus::rtu::max_adu_size,
		"Modbus RTU storage must provide one complete 256-byte ADU");
	static_assert(CrcT::wire_size <= modbus::rtu::max_adu_size - 2u,
		"CRC wire_size leaves no room for RTU address and function");

public:
	using StorageType = StorageT;
	using CrcType = CrcT;
	using Format = modbus::rtu::Format<CrcT::wire_size>;
	using Message = modbus::rtu::Message<StorageT, Format>;
	using Packet = modbus::rtu::Packet<StorageT, Format>;

	static constexpr std::size_t crc_size = Format::crc_size;
	static constexpr std::size_t max_receive_size = Format::max_data_size;
	static constexpr std::size_t max_send_size = Format::max_data_size;
	static constexpr std::size_t max_frame_size = StorageT::max_adu_size;
	static constexpr std::size_t default_capacity_hint =
		max_send_size < 32u ? max_send_size : 32u;

	using Sender = tiny::delegate<bool(std::span<const uint8_t>)>;
	using BusyQuery = tiny::delegate<bool()>;

	Endpoint() noexcept(
			std::is_nothrow_default_constructible_v<StorageT> &&
			std::is_nothrow_default_constructible_v<CrcT>)
		requires std::default_initializable<StorageT> &&
		         std::default_initializable<CrcT>
		= default;

	explicit Endpoint(CrcT crc) noexcept(
			std::is_nothrow_default_constructible_v<StorageT> &&
			std::is_nothrow_move_constructible_v<CrcT>)
		requires std::default_initializable<StorageT> &&
		         std::constructible_from<CrcT, CrcT&&>
		: m_crc(std::move(crc)) {}

	template<class... Args>
	explicit Endpoint(std::in_place_t, Args&&... args) noexcept(
			std::is_nothrow_constructible_v<StorageT, Args&&...> &&
			std::is_nothrow_default_constructible_v<CrcT>)
		requires std::constructible_from<StorageT, Args&&...> &&
		         std::default_initializable<CrcT>
		: m_storage(std::forward<Args>(args)...) {}

	template<class... Args>
	Endpoint(CrcT crc,
	         std::in_place_t,
	         Args&&... args) noexcept(
			std::is_nothrow_constructible_v<StorageT, Args&&...> &&
			std::is_nothrow_move_constructible_v<CrcT>)
		requires std::constructible_from<StorageT, Args&&...> &&
		         std::constructible_from<CrcT, CrcT&&>
		: m_storage(std::forward<Args>(args)...),
		  m_crc(std::move(crc)) {}

	Endpoint(const Endpoint&) = delete;
	Endpoint& operator=(const Endpoint&) = delete;
	Endpoint(Endpoint&&) = delete;
	Endpoint& operator=(Endpoint&&) = delete;

	~Endpoint()
	{
		if (m_active_tx.memory != nullptr) {
			m_storage.release_tx(m_active_tx);
			m_active_tx = {};
		}
	}

	[[nodiscard]] bool bind(Sender sender, BusyQuery busy) noexcept
	{
		if (m_active_tx.memory != nullptr) {
			return false;
		}
		return m_transport.bind(
			static_cast<Sender&&>(sender), static_cast<BusyQuery&&>(busy));
	}

	[[nodiscard]] bool unbind() noexcept
	{
		if (m_active_tx.memory != nullptr) {
			return false;
		}
		m_transport.unbind();
		return true;
	}

	void receive_adu(const std::span<const uint8_t> candidate) noexcept
	{
		m_receiver.receive_adu(m_crc, candidate);
	}

	void notify_gap() noexcept { m_receiver.notify_gap(); }

	[[nodiscard]] Packet pop_packet() noexcept { return m_receiver.pop_packet(); }
	[[nodiscard]] bool has_packet() const noexcept { return m_receiver.has_packet(); }

	[[nodiscard]] Message make_message(
			const uint8_t address,
			const uint8_t function) noexcept
	{
		return make_message(address, function, default_capacity_hint);
	}

	[[nodiscard]] Message make_message(
			const uint8_t address,
			const uint8_t function,
			const std::size_t capacity_hint) noexcept
	{
		if (capacity_hint > max_send_size) {
			return {};
		}
		return Message{m_storage, address, function, capacity_hint};
	}

	[[nodiscard]] SendResult send(Message& message) noexcept
	{
		if (!message || !message.belongs_to(m_storage)) {
			return SendResult::Invalid;
		}
		if (!m_transport.bound()) {
			return SendResult::Unbound;
		}
		if (m_active_tx.memory != nullptr || m_transport.busy()) {
			++m_tx_stats.send_refused_busy;
			return SendResult::Busy;
		}

		const std::span<const uint8_t> adu = message.finalize(m_crc);
		if (adu.empty()) {
			return SendResult::Invalid;
		}
		if (!m_transport.start(adu)) {
			++m_tx_stats.send_failed;
			return SendResult::Failed;
		}

		m_active_tx = message.surrender_block();
		++m_tx_stats.frames_sent;
		return SendResult::Sent;
	}

	[[nodiscard]] bool tx_active() const noexcept
	{
		return m_active_tx.memory != nullptr;
	}

	void poll() noexcept
	{
		if (m_active_tx.memory != nullptr && !m_transport.busy()) {
			m_storage.release_tx(m_active_tx);
			m_active_tx = {};
		}
	}

	[[nodiscard]] modbus::rtu::Stats stats() const noexcept
	{
		return {m_receiver.stats(), m_tx_stats};
	}

	[[nodiscard]] const StorageT& storage() const noexcept { return m_storage; }

private:
	class Transport final {
	public:
		[[nodiscard]] bool bind(Sender sender, BusyQuery busy) noexcept
		{
			if (!sender || !busy) {
				return false;
			}
			m_sender = static_cast<Sender&&>(sender);
			m_busy = static_cast<BusyQuery&&>(busy);
			return true;
		}

		void unbind() noexcept
		{
			m_sender = nullptr;
			m_busy = nullptr;
		}

		[[nodiscard]] bool bound() const noexcept
		{
			return static_cast<bool>(m_sender) && static_cast<bool>(m_busy);
		}

		[[nodiscard]] bool busy() const noexcept { return m_busy(); }

		[[nodiscard]] bool start(
				const std::span<const uint8_t> adu) const noexcept
		{
			return m_sender(adu);
		}

	private:
		Sender m_sender{};
		BusyQuery m_busy{};
	};

	[[no_unique_address]] StorageT m_storage{};
	detail::Receiver<StorageT, Format> m_receiver{m_storage};
	Transport m_transport{};
	TxBlock m_active_tx{};
	modbus::rtu::Stats::Tx m_tx_stats{};
	// Keep policy last: a pointer-sized state fits existing Cortex-M tail padding.
	[[no_unique_address]] CrcT m_crc{};
};

} // namespace modbus::rtu

#endif /* MODBUS_RTU_H_ */
