/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Modbus RTU endpoint.
 *
 *     modbus::rtu::Endpoint<Memory, Format, Framer = framing::None>
 *
 * Memory is a wire::Storage specification (wire::Heap, wire::Pool<Rx, Tx>, or
 * a user-written one) — the same type a cobs::Endpoint accepts, because
 * storage knows nothing about either protocol. Format names the integrity
 * policy and the physical ADU ceiling; the endpoint derives its block
 * geometry from the Format's Layout and binds the memory to it.
 *
 * Ownership and transport semantics deliberately mirror cobs::Endpoint.
 * The RX boundary is different: receive_adu() accepts exactly one physical
 * UART burst candidate, not arbitrary stream chunks. The candidate is
 * validated before one copy into Packet-owned storage.
 *
 * Framer is optional (Framing.h). With the default, framing::None, nothing
 * above changes: no function code is interpreted, and the endpoint's layout
 * and code paths are those of the two-parameter endpoint. With a framing
 * policy the endpoint gains consume() for arbitrary stream chunks, accepts
 * only functions the policy has a layout for, fills a policy-declared length
 * prefix on send, and refuses to send data that disagrees with its layout.
 */

#ifndef MODBUS_RTU_H_
#define MODBUS_RTU_H_

#include "../Pdu.h"
#include "../../crc/Crc.h"
#include "../../wire/Storage.h"
#include "Format.h"
#include "Framing.h"
#include "Stats.h"
#include "detail/Message.h"
#include "detail/Packet.h"
#include "detail/Receiver.h"
#include "detail/RxBlock.h"
#include "detail/StreamReceiver.h"

#include "tiny_delegate.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

namespace modbus::rtu {

using SendResult = modbus::SendResult;

template<class MemoryT = wire::Heap,
         class FormatT = modbus::rtu::Format<>,
         class FramerT = framing::None>
class Endpoint final {
	static_assert(framing::Framer<FramerT>,
		"Endpoint framer must be framing::None or satisfy framing::Policy");

public:
	using Memory = MemoryT;
	using Format = FormatT;
	using Framer = FramerT;
	using Crc = typename Format::Crc;
	using Layout = typename Format::Layout;

	// Whether a framing policy is in effect. It never changes Packet, Message,
	// Storage or the wire; it adds consume() and the layout table.
	static constexpr bool framed = !std::is_same_v<FramerT, framing::None>;

	/*
	 * The physical block geometry this endpoint binds its memory to
	 * (detail/RxBlock.h): a wire::BlockGeometry keyed on the numbers alone,
	 * so every Format with the same Layout — Crc16Bitwise and Crc16Table,
	 * say — binds the same storage type and shares Packet, Message and
	 * Receiver.
	 */
	using Geometry = detail::GeometryFor<Layout>;

	static_assert(wire::Storage<MemoryT, Geometry>,
		"Endpoint storage must satisfy the wire::Storage contract");

	using Storage = typename MemoryT::template For<Geometry>;

private:
	// Layout-only stand-in for the storage-typed RX block; see cobs/Cobs.h.
	using Block = modbus::rtu::RxBlock<Storage>;
	using Shape = modbus::rtu::RxBlock<detail::AnyStorage>;
	static_assert(sizeof(Block) == sizeof(Shape) && alignof(Block) == alignof(Shape),
		"the RX block header must have the layout the geometry was sized from");

	using Receiver = std::conditional_t<framed,
		detail::StreamReceiver<Storage, Layout, FramerT>,
		detail::Receiver<Storage, Layout>>;

public:

	using Message = modbus::rtu::Message<Storage, Layout>;
	using Packet = modbus::rtu::Packet<Storage, Layout>;

	static constexpr std::size_t crc_size = Layout::crc_size;
	static constexpr std::size_t max_receive_size = Layout::max_data_size;
	static constexpr std::size_t max_send_size = Layout::max_data_size;
	static constexpr std::size_t max_frame_size = Layout::max_adu_size;
	static constexpr std::size_t default_capacity_hint =
		max_send_size < 32u ? max_send_size : 32u;

	using Sender = tiny::delegate<bool(std::span<const uint8_t>)>;
	using BusyQuery = tiny::delegate<bool()>;

	Endpoint() noexcept(
			std::is_nothrow_default_constructible_v<Storage> &&
			std::is_nothrow_default_constructible_v<Crc>)
		requires std::default_initializable<Storage> &&
		         std::default_initializable<Crc>
		= default;

	explicit Endpoint(Crc crc) noexcept(
			std::is_nothrow_default_constructible_v<Storage> &&
			std::is_nothrow_move_constructible_v<Crc>)
		requires std::default_initializable<Storage> &&
		         std::constructible_from<Crc, Crc&&>
		: m_crc(std::move(crc)) {}

	template<class... Args>
	explicit Endpoint(std::in_place_t, Args&&... args) noexcept(
			std::is_nothrow_constructible_v<Storage, Args&&...> &&
			std::is_nothrow_default_constructible_v<Crc>)
		requires std::constructible_from<Storage, Args&&...> &&
		         std::default_initializable<Crc>
		: m_storage(std::forward<Args>(args)...) {}

	template<class... Args>
	Endpoint(Crc crc,
	         std::in_place_t,
	         Args&&... args) noexcept(
			std::is_nothrow_constructible_v<Storage, Args&&...> &&
			std::is_nothrow_move_constructible_v<Crc>)
		requires std::constructible_from<Storage, Args&&...> &&
		         std::constructible_from<Crc, Crc&&>
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

	// Arbitrary stream chunks: a fragment of an ADU, several ADUs, or both.
	// Only with a framing policy; the frame end is found from its layout.
	void consume(const std::span<const uint8_t> bytes) noexcept
		requires framed
	{
		m_receiver.consume(m_crc, bytes);
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
		if constexpr (framed) {
			// The policy's TX-side layout: a library-owned length prefix is
			// reserved now and filled at send(); a fixed size is at least
			// pre-allocated so the message need not grow to reach it.
			const framing::Layout layout = tx_layout(function);
			std::size_t hint = capacity_hint;
			if (layout.kind == framing::Layout::Kind::Fixed &&
			    hint < layout.size && layout.size <= max_send_size) {
				hint = layout.size;
			}
			if (hint < layout.reserved()) {
				hint = layout.reserved();
			}
			return Message{m_storage, address, function, hint, layout.reserved()};
		} else {
			return Message{m_storage, address, function, capacity_hint};
		}
	}

	[[nodiscard]] SendResult send(Message& message) noexcept
	{
		if (!message || !message.belongs_to(m_storage)) {
			return SendResult::Invalid;
		}
		if constexpr (framed) {
			if (!message.apply_framing(tx_layout(message.function()))) {
				m_receiver.note_tx_layout_rejected();
				return SendResult::Invalid;
			}
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

	[[nodiscard]] const modbus::rtu::FramingStats& framing_stats() const noexcept
		requires framed
	{
		return m_receiver.framing_stats();
	}

	[[nodiscard]] const Storage& storage() const noexcept { return m_storage; }

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

	// What this endpoint SENDS is the other side of what it receives.
	[[nodiscard]] static constexpr framing::Layout tx_layout(
			const uint8_t function) noexcept
		requires framed
	{
		return Framer::layout(framing::opposite(Framer::rx), function);
	}

	[[no_unique_address]] Storage m_storage{};
	Receiver m_receiver{m_storage};
	Transport m_transport{};
	wire::TxBlock m_active_tx{};
	modbus::rtu::Stats::Tx m_tx_stats{};
	// Keep policy last: a pointer-sized state fits existing Cortex-M tail padding.
	[[no_unique_address]] Crc m_crc{};
};

} // namespace modbus::rtu

#endif /* MODBUS_RTU_H_ */
