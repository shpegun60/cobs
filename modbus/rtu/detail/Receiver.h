/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* One receive call is one UART-burst-delimited candidate RTU ADU. */

#ifndef MODBUS_RTU_DETAIL_RECEIVER_H_
#define MODBUS_RTU_DETAIL_RECEIVER_H_

#include "../Crc.h"
#include "../Stats.h"
#include "Packet.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace modbus::rtu::detail {

template<class StorageT>
class Receiver final {
	static_assert(modbus::rtu::Storage<StorageT>,
		"Receiver storage must satisfy modbus::rtu::Storage");
	static_assert(StorageT::max_adu_size == modbus::rtu::max_adu_size &&
	              StorageT::max_data_size == modbus::max_data_size,
		"Modbus RTU storage must provide the complete fixed RTU geometry");

public:
	using Block = typename StorageT::RxBlock;
	using Packet = modbus::rtu::Packet<StorageT>;

	explicit Receiver(StorageT& storage) noexcept : m_storage(storage) {}
	Receiver(const Receiver&) = delete;
	Receiver& operator=(const Receiver&) = delete;

	~Receiver() { clear_ready(); }

	template<modbus::rtu::crc::Calculator CrcT>
	void receive_adu(
			CrcT& calculator,
			const std::span<const uint8_t> candidate) noexcept
	{
		++m_stats.candidates;
		if (candidate.size() < min_adu_size) {
			++m_stats.too_short;
			return;
		}
		if (candidate.size() > max_adu_size) {
			++m_stats.oversize;
			return;
		}
		if (!modbus::rtu::crc::verify(candidate, calculator)) {
			++m_stats.crc_errors;
			return;
		}

		Block* const block = m_storage.acquire_rx(candidate.size());
		if (block == nullptr) {
			++m_stats.allocation_failure;
			return;
		}
		block->owner = &m_storage;
		block->adu_size = static_cast<uint16_t>(candidate.size());
		block->address = candidate[0];
		block->function = candidate[address_size];
		std::memcpy(block->writable_adu(candidate.size()).data(),
		            candidate.data(), candidate.size());
		enqueue(block);
		++m_stats.frames_received;
	}

	// The current UART adapter never hands a partial burst to this receiver on
	// loss. The ordered notification is therefore diagnostic state, not a
	// request for a byte scanner or a COBS-style resynchronization mode.
	void notify_gap() noexcept { ++m_stats.stream_gaps; }

	[[nodiscard]] Packet pop_packet() noexcept
	{
		Block* const block = dequeue();
		return block != nullptr ? Packet::adopt(block) : Packet{};
	}

	[[nodiscard]] bool has_packet() const noexcept { return m_head != nullptr; }
	[[nodiscard]] const modbus::rtu::Stats::Rx& stats() const noexcept
	{
		return m_stats;
	}

private:
	void enqueue(Block* const block) noexcept
	{
		block->next_ready = nullptr;
		if (m_tail != nullptr) {
			m_tail->next_ready = block;
		} else {
			m_head = block;
		}
		m_tail = block;
	}

	[[nodiscard]] Block* dequeue() noexcept
	{
		Block* const block = m_head;
		if (block != nullptr) {
			m_head = block->next_ready;
			if (m_head == nullptr) {
				m_tail = nullptr;
			}
			block->next_ready = nullptr;
		}
		return block;
	}

	void clear_ready() noexcept
	{
		while (Block* const block = dequeue()) {
			(void)Packet::adopt(block);
		}
	}

	StorageT& m_storage;
	Block* m_head = nullptr;
	Block* m_tail = nullptr;
	modbus::rtu::Stats::Rx m_stats{};
};

} // namespace modbus::rtu::detail

#endif /* MODBUS_RTU_DETAIL_RECEIVER_H_ */
