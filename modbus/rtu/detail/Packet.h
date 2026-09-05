/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Immutable, copyable intrusive owner of one validated Modbus RTU ADU. */

#ifndef MODBUS_RTU_DETAIL_PACKET_H_
#define MODBUS_RTU_DETAIL_PACKET_H_

#include "../Format.h"
#include "RxBlock.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace modbus::rtu {

// Typed on the instantiated storage (the block's owner pointer) and on the
// LAYOUT, not the Format: policies of equal trailer width share this type.
template<class StorageT, class LayoutT>
class Packet final {
	template<class, class>
	friend class detail::Receiver;

public:
	using Block = modbus::rtu::RxBlock<StorageT>;
	using Layout = LayoutT;

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
		// Increment BEFORE releasing: with self-assignment both sides are the
		// same packet, and releasing first could free what we then adopt.
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
			? std::span<const uint8_t>{
				m_block->payload() + Layout::adu_prefix_size,
				static_cast<std::size_t>(m_block->adu_size) -
					Layout::adu_overhead}
			: std::span<const uint8_t>{};
	}

	[[nodiscard]] std::size_t size() const noexcept { return data().size(); }

	[[nodiscard]] std::span<const uint8_t> pdu() const noexcept
	{
		return m_block != nullptr
			? std::span<const uint8_t>{
				m_block->payload() + Layout::address_size,
				static_cast<std::size_t>(m_block->adu_size) -
					Layout::pdu_envelope_size}
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
			// Trivially destructible: the bytes go straight back to the
			// storage that handed them out.
			m_block->owner->release_rx(
				static_cast<std::byte*>(static_cast<void*>(m_block)));
		}
	}

	Block* m_block = nullptr;
};

} // namespace modbus::rtu

#endif /* MODBUS_RTU_DETAIL_PACKET_H_ */
