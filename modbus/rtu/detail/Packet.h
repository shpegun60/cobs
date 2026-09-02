/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Immutable, copyable intrusive owner of one validated Modbus RTU ADU. */

#ifndef MODBUS_RTU_DETAIL_PACKET_H_
#define MODBUS_RTU_DETAIL_PACKET_H_

#include "../Storage.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace modbus::rtu {

template<class StorageT>
class Packet final {
	template<class>
	friend class detail::Receiver;

public:
	using Block = typename StorageT::RxBlock;

	Packet() noexcept = default;
	~Packet() { release(); }

	Packet(const Packet& other) noexcept : m_block(other.m_block)
	{
		if (m_block != nullptr) {
			++m_block->refs;
		}
	}

	Packet(Packet&& other) noexcept : m_block(other.m_block)
	{
		other.m_block = nullptr;
	}

	Packet& operator=(const Packet& other) noexcept
	{
		if (other.m_block != nullptr) {
			++other.m_block->refs;
		}
		release();
		m_block = other.m_block;
		return *this;
	}

	Packet& operator=(Packet&& other) noexcept
	{
		if (this != &other) {
			release();
			m_block = other.m_block;
			other.m_block = nullptr;
		}
		return *this;
	}

	void reset() noexcept
	{
		release();
		m_block = nullptr;
	}

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return m_block != nullptr;
	}

	[[nodiscard]] uint8_t address() const noexcept
	{
		return m_block != nullptr ? m_block->address : 0u;
	}

	[[nodiscard]] uint8_t function() const noexcept
	{
		return m_block != nullptr ? m_block->function : 0u;
	}

	// Same application-facing meaning as cobs::Packet::data()/size(): only
	// bytes supplied by the message builder, never transport metadata.
	[[nodiscard]] std::span<const uint8_t> data() const noexcept
	{
		return m_block != nullptr
			? std::span<const uint8_t>{m_block->payload() + adu_prefix_size,
				static_cast<std::size_t>(m_block->adu_size) - adu_overhead}
			: std::span<const uint8_t>{};
	}

	[[nodiscard]] std::size_t size() const noexcept { return data().size(); }

	[[nodiscard]] std::span<const uint8_t> pdu() const noexcept
	{
		return m_block != nullptr
			? std::span<const uint8_t>{m_block->payload() + address_size,
				static_cast<std::size_t>(m_block->adu_size) - pdu_envelope_size}
			: std::span<const uint8_t>{};
	}

	[[nodiscard]] std::span<const uint8_t> adu() const noexcept
	{
		return m_block != nullptr
			? std::span<const uint8_t>{m_block->payload(), m_block->adu_size}
			: std::span<const uint8_t>{};
	}

private:
	[[nodiscard]] static Packet adopt(Block* const block) noexcept
	{
		Packet packet;
		packet.m_block = block;
		return packet;
	}

	void release() noexcept
	{
		if (m_block != nullptr && --m_block->refs == 0u) {
			m_block->owner->release_rx(m_block);
		}
	}

	Block* m_block = nullptr;
};

} // namespace modbus::rtu

#endif /* MODBUS_RTU_DETAIL_PACKET_H_ */
