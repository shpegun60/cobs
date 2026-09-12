/* Author: shpegun60; SPDX-License-Identifier: MIT */
#ifndef MODBUS_TCP_DETAIL_PACKET_H_
#define MODBUS_TCP_DETAIL_PACKET_H_
#include "RxBlock.h"
#include "../../../wire/Scalar.h"
#include <span>

namespace modbus::tcp {
// Immutable shared ownership; reference counting is deliberately non-atomic.
template<class StorageT, class LayoutT>
class Packet final {
	template<class, class> friend class detail::Receiver;
public:
	using Block = detail::RxBlock<StorageT>;
	using Layout = LayoutT;
	Packet() noexcept = default;
	~Packet() { release(); }
	Packet(const Packet& other) noexcept : m_block(other.m_block)
	{
		if (m_block != nullptr) { ++m_block->refs; }
	}
	Packet(Packet&& other) noexcept : m_block(other.m_block) { other.m_block = nullptr; }
	Packet& operator=(const Packet& other) noexcept
	{
		if (other.m_block != nullptr) { ++other.m_block->refs; }
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
	void reset() noexcept { release(); m_block = nullptr; }
	[[nodiscard]] explicit operator bool() const noexcept { return m_block != nullptr; }
	[[nodiscard]] uint16_t transaction_id() const noexcept
	{
		return m_block != nullptr
			? wire::detail::load_ordered<std::endian::big, uint16_t>(m_block->payload()) : 0u;
	}
	[[nodiscard]] uint8_t unit_id() const noexcept
	{
		return m_block != nullptr ? m_block->payload()[6] : 0u;
	}
	[[nodiscard]] uint8_t function() const noexcept
	{
		return m_block != nullptr ? m_block->payload()[7] : 0u;
	}
	[[nodiscard]] std::span<const uint8_t> data() const noexcept
	{
		return m_block != nullptr ? std::span<const uint8_t>{
			m_block->payload() + Layout::adu_prefix_size,
			m_block->adu_size - Layout::adu_overhead} : std::span<const uint8_t>{};
	}
	[[nodiscard]] std::size_t size() const noexcept { return data().size(); }
	[[nodiscard]] std::span<const uint8_t> pdu() const noexcept
	{
		return m_block != nullptr ? std::span<const uint8_t>{
			m_block->payload() + Layout::mbap_size,
			m_block->adu_size - Layout::pdu_envelope_size} : std::span<const uint8_t>{};
	}
	[[nodiscard]] std::span<const uint8_t> adu() const noexcept
	{
		return m_block != nullptr ? std::span<const uint8_t>{
			m_block->payload(), m_block->adu_size} : std::span<const uint8_t>{};
	}
private:
	[[nodiscard]] static Packet adopt(Block* const block) noexcept
	{
		Packet result;
		result.m_block = block;
		return result;
	}
	void release() noexcept
	{
		if (m_block != nullptr && --m_block->refs == 0u) {
			m_block->owner->release_rx(static_cast<std::byte*>(static_cast<void*>(m_block)));
		}
	}
	Block* m_block = nullptr;
};
} // namespace modbus::tcp
#endif
