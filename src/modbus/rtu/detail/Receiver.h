/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * One receive call is one UART-burst-delimited candidate RTU ADU. This is the
 * whole RX vertical of the default endpoint; StreamReceiver.h derives from it
 * to add stream assembly when a framing policy is selected, so what is
 * protected here is exactly what that derivation needs and nothing more.
 */

#ifndef MODBUS_RTU_DETAIL_RECEIVER_H_
#define MODBUS_RTU_DETAIL_RECEIVER_H_

#include "../Format.h"
#include "../Stats.h"
#include "../../../wire/Storage.h"
#include "Packet.h"
#include "RxBlock.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <type_traits>

namespace modbus::rtu::detail {

template<class StorageT, class LayoutT>
class Receiver {
	static_assert(wire::ByteStorage<StorageT>,
		"Receiver storage must satisfy the wire::ByteStorage contract");

public:
	using Storage = StorageT;
	using Block = modbus::rtu::RxBlock<StorageT>;
	using Layout = LayoutT;
	using Packet = modbus::rtu::Packet<StorageT, Layout>;

	// Storage hands back bytes, not objects, so nothing may need tearing down
	// before those bytes go back.
	static_assert(std::is_trivially_destructible_v<Block>,
		"RxBlock must stay trivially destructible: storage releases raw bytes");

	explicit Receiver(StorageT& storage) noexcept : m_storage(storage) {}
	Receiver(const Receiver&) = delete;
	Receiver& operator=(const Receiver&) = delete;

	~Receiver() { clear_ready(); }

	template<::crc::Policy CrcT>
		requires (CrcT::wire_size == Layout::crc_size)
	void receive_adu(
			CrcT& policy,
			const std::span<const uint8_t> candidate) noexcept
	{
		++m_stats.candidates;
		if (candidate.size() < Layout::min_adu_size) {
			++m_stats.too_short;
			return;
		}
		if (candidate.size() > Layout::max_adu_size) {
			++m_stats.oversize;
			return;
		}
		if (!::crc::verify(candidate, policy)) {
			++m_stats.crc_errors;
			return;
		}

		Block* const block = acquire_block(candidate.size());
		if (block == nullptr) {
			++m_stats.allocation_failure;
			return;
		}
		block->adu_size = static_cast<uint16_t>(candidate.size());
		block->address = candidate[0];
		block->function = candidate[Layout::address_size];
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

protected:
	// Storage supplies sizeof(Block) + adu bytes and knows nothing about what
	// goes in them; the header is constructed here and ownership is
	// established here. The only place a block comes into being.
	[[nodiscard]] Block* acquire_block(const std::size_t adu) noexcept
	{
		std::byte* const memory = m_storage.acquire_rx(sizeof(Block) + adu);
		if (memory == nullptr) {
			return nullptr;
		}
		Block* const block = std::construct_at(
			static_cast<Block*>(static_cast<void*>(memory)));
		block->owner = &m_storage;
		return block;
	}

	[[nodiscard]] static std::byte* bytes_of(Block* const block) noexcept
	{
		return static_cast<std::byte*>(static_cast<void*>(block));
	}

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

	StorageT& m_storage;
	modbus::rtu::Stats::Rx m_stats{};

private:
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

	Block* m_head = nullptr;
	Block* m_tail = nullptr;
};

} // namespace modbus::rtu::detail

#endif /* MODBUS_RTU_DETAIL_RECEIVER_H_ */
