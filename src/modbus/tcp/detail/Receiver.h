/* Author: shpegun60; SPDX-License-Identifier: MIT */
#ifndef MODBUS_TCP_DETAIL_RECEIVER_H_
#define MODBUS_TCP_DETAIL_RECEIVER_H_
#include "../Format.h"
#include "../Stats.h"
#include "Packet.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <type_traits>

namespace modbus::tcp::detail {
// MBAP is the ONLY framer. Six staging bytes suffice regardless of ADU limit.
template<class StorageT, class LayoutT>
class Receiver final {
	static_assert(wire::ByteStorage<StorageT>);
public:
	using Layout = LayoutT;
	using Block = RxBlock<StorageT>;
	using Packet = modbus::tcp::Packet<StorageT, Layout>;
	static_assert(std::is_trivially_destructible_v<Block>);
	explicit Receiver(StorageT& storage) noexcept : m_storage(storage) {}
	Receiver(const Receiver&) = delete;
	Receiver& operator=(const Receiver&) = delete;
	~Receiver()
	{
		reset_rx();
		while (has_packet()) { (void)pop_packet(); }
	}
	template<::crc::Policy CrcT> requires (CrcT::wire_size == Layout::crc_size)
	void consume(CrcT& policy, std::span<const uint8_t> bytes) noexcept
	{
		while (!bytes.empty() && m_state != State::Failed) {
			if (m_state == State::Header) {
				const std::size_t count = std::min(bytes.size(), m_header.size() - m_header_used);
				std::memcpy(m_header.data() + m_header_used, bytes.data(), count);
				m_header_used += count;
				bytes = bytes.subspan(count);
				if (m_header_used == m_header.size()) { begin_frame(); }
			} else {
				const std::size_t count = std::min(bytes.size(), m_remaining);
				if (m_state == State::Body) {
					std::memcpy(m_building->payload() + m_building->adu_size - m_remaining,
						bytes.data(), count);
				}
				m_remaining -= count;
				bytes = bytes.subspan(count);
				if (m_remaining == 0u) {
					if (m_state == State::Body) { finish_frame(policy); }
					else { ++m_stats.skipped_frames; m_state = State::Header; }
					m_header_used = 0u;
				}
			}
		}
	}
	// Reset is legal ONLY at a caller-guaranteed new stream boundary.
	void reset_rx() noexcept
	{
		if (m_building != nullptr) {
			m_storage.release_rx(bytes_of(m_building));
			m_building = nullptr;
		}
		m_header_used = 0u;
		m_remaining = 0u;
		m_state = State::Header;
	}
	void notify_gap() noexcept
	{
		reset_rx();
		m_state = State::Failed;
		++m_stats.stream_gaps;
	}
	[[nodiscard]] bool rx_failed() const noexcept { return m_state == State::Failed; }
	[[nodiscard]] bool assembling() const noexcept
	{
		return !rx_failed() && (m_state != State::Header || m_header_used != 0u);
	}
	[[nodiscard]] bool has_packet() const noexcept { return m_head != nullptr; }
	[[nodiscard]] Packet pop_packet() noexcept
	{
		Block* const block = m_head;
		if (block == nullptr) { return {}; }
		m_head = block->next_ready;
		if (m_head == nullptr) { m_tail = nullptr; }
		block->next_ready = nullptr;
		return Packet::adopt(block);
	}
	[[nodiscard]] const Stats::Rx& stats() const noexcept { return m_stats; }
private:
	enum class State : uint8_t { Header, Body, Skip, Failed };
	void begin_frame() noexcept
	{
		++m_stats.candidates;
		if (m_header[2] != 0u || m_header[3] != 0u) {
			++m_stats.invalid_protocol;
			m_state = State::Failed;
			return;
		}
		const std::size_t length = wire::detail::load_ordered<std::endian::big, uint16_t>(m_header.data() + 4u);
		const std::size_t total = Layout::length_prefix_size + length;
		if (total < Layout::min_adu_size) {
			++m_stats.invalid_length;
			m_state = State::Failed;
			return;
		}
		if (total > Layout::max_adu_size) {
			++m_stats.oversize;
			m_state = State::Failed;
			return;
		}
		m_remaining = length;
		std::byte* const memory = m_storage.acquire_rx(sizeof(Block) + total);
		if (memory == nullptr) {
			++m_stats.allocation_failure;
			m_state = State::Skip;
			return;
		}
		m_building = std::construct_at(static_cast<Block*>(static_cast<void*>(memory)));
		m_building->owner = &m_storage;
		m_building->adu_size = static_cast<uint32_t>(total);
		std::memcpy(m_building->payload(), m_header.data(), m_header.size());
		m_state = State::Body;
	}
	template<::crc::Policy CrcT>
	void finish_frame(CrcT& policy) noexcept
	{
		Block* const block = m_building;
		m_building = nullptr;
		if (!::crc::verify({block->payload(), block->adu_size}, policy)) {
			m_storage.release_rx(bytes_of(block));
			++m_stats.crc_errors;
			// Even a plausible Length may be corrupt. Never guess the next boundary.
			m_state = State::Failed;
			return;
		}
		if (m_tail != nullptr) { m_tail->next_ready = block; }
		else { m_head = block; }
		m_tail = block;
		++m_stats.frames_received;
		m_state = State::Header;
	}
	[[nodiscard]] static std::byte* bytes_of(Block* const block) noexcept
	{
		return static_cast<std::byte*>(static_cast<void*>(block));
	}
	StorageT& m_storage;
	Block* m_head = nullptr;
	Block* m_tail = nullptr;
	Block* m_building = nullptr;
	std::size_t m_remaining = 0u;
	std::size_t m_header_used = 0u;
	Stats::Rx m_stats{};
	std::array<uint8_t, Layout::length_prefix_size> m_header{};
	State m_state = State::Header;
};
} // namespace modbus::tcp::detail
#endif
