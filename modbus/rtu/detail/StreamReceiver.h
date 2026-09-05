/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * modbus::rtu::detail::StreamReceiver — the RX vertical of an endpoint with a
 * framing policy. It is the burst receiver plus the two-stage assembly COBS
 * already uses:
 *
 *     Prefix   address + function into a small local buffer
 *     Header   the rest of the bytes the function's Layout needs
 *              -> data size known -> acquire_rx(header + adu), exactly
 *     Body     the remaining bytes copied straight into their final block
 *              -> CRC on the complete ADU in place -> publish
 *     Skip     the block could not be allocated: the declared frame is
 *              skipped byte-exactly and synchronization is kept
 *
 * There is no staging buffer for the ADU and no copy after allocation. The
 * only temporary storage is the 16-byte prefix buffer. Every other error
 * (unsupported function, oversize declaration, CRC failure) drops the rest
 * of the current chunk: see the recovery rule in ../Framing.h.
 *
 * receive_adu() remains available and, with a framing policy, applies the
 * same layout table to the complete candidate before the burst receiver
 * validates it.
 */

#ifndef MODBUS_RTU_DETAIL_STREAM_RECEIVER_H_
#define MODBUS_RTU_DETAIL_STREAM_RECEIVER_H_

#include "../Framing.h"
#include "../Stats.h"
#include "Receiver.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace modbus::rtu::detail {

template<class StorageT, class LayoutT, class FramerT>
class StreamReceiver final : public Receiver<StorageT, LayoutT> {
	static_assert(framing::Policy<FramerT>,
		"StreamReceiver needs a framing policy: a Direction `rx` and layout()");
	using Base = Receiver<StorageT, LayoutT>;

public:
	using Storage = StorageT;
	using Layout = LayoutT;
	using Framer = FramerT;
	using Block = typename Base::Block;

	static constexpr std::size_t prefix_capacity =
		Layout::adu_prefix_size + framing::Layout::max_header_size;

	explicit StreamReceiver(StorageT& storage) noexcept : Base(storage) {}
	~StreamReceiver() { release_building(); }

	template<::crc::Policy CrcT>
		requires (CrcT::wire_size == Layout::crc_size)
	void consume(CrcT& policy, std::span<const uint8_t> bytes) noexcept
	{
		while (!bytes.empty()) {
			switch (m_stage) {
			case Stage::Prefix:
			case Stage::Header:
				if (!gather(bytes)) {
					return; // the rest of the chunk was discarded
				}
				if (m_stage == Stage::Body && m_remaining == 0u && !complete(policy)) {
					drop_rest(bytes);
					return;
				}
				break;
			case Stage::Body: {
				const std::size_t take =
					m_remaining < bytes.size() ? m_remaining : bytes.size();
				std::memcpy(m_building->payload() + m_filled, bytes.data(), take);
				m_filled = static_cast<uint16_t>(m_filled + take);
				m_remaining = static_cast<uint16_t>(m_remaining - take);
				bytes = bytes.subspan(take);
				if (m_remaining == 0u && !complete(policy)) {
					drop_rest(bytes);
					return;
				}
				break;
			}
			case Stage::Skip: {
				const std::size_t take =
					m_remaining < bytes.size() ? m_remaining : bytes.size();
				m_remaining = static_cast<uint16_t>(m_remaining - take);
				bytes = bytes.subspan(take);
				if (m_remaining == 0u) {
					reset();
				}
				break;
			}
			}
		}
	}

	// A complete candidate, as with framing::None, but only a function the
	// policy has a layout for, and only when its length agrees with it.
	template<::crc::Policy CrcT>
		requires (CrcT::wire_size == Layout::crc_size)
	void receive_adu(
			CrcT& policy,
			const std::span<const uint8_t> candidate) noexcept
	{
		if (candidate.size() >= Layout::min_adu_size &&
		    candidate.size() <= Layout::max_adu_size) {
			const framing::Layout layout =
				Framer::layout(Framer::rx, candidate[Layout::address_size]);
			if (!layout.supported()) {
				++Base::m_stats.candidates;
				++m_framing.unsupported_function;
				return;
			}
			if (!layout.matches(candidate.subspan(
					Layout::adu_prefix_size,
					candidate.size() - Layout::adu_overhead))) {
				++Base::m_stats.candidates;
				++m_framing.length_mismatch;
				return;
			}
		}
		Base::receive_adu(policy, candidate);
	}

	// Bytes were lost: whatever frame was in flight cannot be completed, and
	// the next chunk starts a new one.
	void notify_gap() noexcept
	{
		release_building();
		reset();
		Base::notify_gap();
	}

	[[nodiscard]] const modbus::rtu::FramingStats& framing_stats() const noexcept
	{
		return m_framing;
	}

	void note_tx_layout_rejected() noexcept { ++m_framing.tx_layout_rejected; }

private:
	enum class Stage : uint8_t { Prefix, Header, Body, Skip };

	// Collects prefix/header bytes; returns false when the chunk's remainder
	// was discarded because the frame was refused.
	[[nodiscard]] bool gather(std::span<const uint8_t>& bytes) noexcept
	{
		const std::size_t take =
			static_cast<std::size_t>(m_need - m_filled) < bytes.size()
				? static_cast<std::size_t>(m_need - m_filled)
				: bytes.size();
		std::memcpy(m_prefix.data() + m_filled, bytes.data(), take);
		m_filled = static_cast<uint16_t>(m_filled + take);
		bytes = bytes.subspan(take);
		if (m_filled < m_need) {
			return true;
		}
		if (m_stage == Stage::Prefix) {
			// Address and function are in: the policy decides how much more
			// header this function needs, or refuses it outright.
			++Base::m_stats.candidates;
			m_layout = Framer::layout(Framer::rx, m_prefix[Layout::address_size]);
			if (!m_layout.supported()) {
				++m_framing.unsupported_function;
				return refuse(bytes);
			}
			m_need = static_cast<uint8_t>(
				Layout::adu_prefix_size + m_layout.header_size());
			m_stage = Stage::Header;
			if (m_filled < m_need) {
				return true;
			}
		}
		return begin_frame(bytes);
	}

	// The header is complete: size the frame, allocate exactly, and move the
	// header bytes to their final home.
	[[nodiscard]] bool begin_frame(std::span<const uint8_t>& bytes) noexcept
	{
		const std::size_t data = m_layout.data_size(std::span<const uint8_t>{
			m_prefix.data() + Layout::adu_prefix_size,
			static_cast<std::size_t>(m_filled) - Layout::adu_prefix_size});
		const std::size_t adu = Layout::adu_size_for_data(data);
		if (adu > Layout::max_adu_size) {
			++Base::m_stats.oversize;
			return refuse(bytes);
		}
		Block* const block = Base::acquire_block(adu);
		if (block == nullptr) {
			// The length is known, so exactly this frame is skipped and the
			// stream stays in step.
			++Base::m_stats.allocation_failure;
			++m_framing.skipped_frames;
			m_remaining = static_cast<uint16_t>(adu - m_filled);
			if (m_remaining == 0u) {
				reset();
			} else {
				m_stage = Stage::Skip;
			}
			return true;
		}
		block->adu_size = static_cast<uint16_t>(adu);
		block->address = m_prefix[0];
		block->function = m_prefix[Layout::address_size];
		std::memcpy(block->payload(), m_prefix.data(), m_filled);
		m_building = block;
		m_remaining = static_cast<uint16_t>(adu - m_filled);
		m_stage = Stage::Body;
		return true;
	}

	// The declared bytes are all in place: the ADU is validated where it lies.
	template<::crc::Policy CrcT>
	[[nodiscard]] bool complete(CrcT& policy) noexcept
	{
		const std::span<const uint8_t> adu{
			m_building->payload(), m_building->adu_size};
		if (!::crc::verify(adu, policy)) {
			++Base::m_stats.crc_errors;
			release_building();
			reset();
			return false;
		}
		Base::enqueue(m_building);
		m_building = nullptr;
		++Base::m_stats.frames_received;
		reset();
		return true;
	}

	[[nodiscard]] bool refuse(std::span<const uint8_t>& bytes) noexcept
	{
		reset();
		drop_rest(bytes);
		return false;
	}

	// After an error the frame start is unknown until the next chunk.
	void drop_rest(std::span<const uint8_t>& bytes) noexcept
	{
		if (!bytes.empty()) {
			++m_framing.resyncs;
			bytes = {};
		}
	}

	void reset() noexcept
	{
		m_stage = Stage::Prefix;
		m_filled = 0u;
		m_remaining = 0u;
		m_need = static_cast<uint8_t>(Layout::adu_prefix_size);
	}

	void release_building() noexcept
	{
		if (m_building != nullptr) {
			Base::m_storage.release_rx(Base::bytes_of(m_building));
			m_building = nullptr;
		}
	}

	Block* m_building = nullptr;
	framing::Layout m_layout{};
	uint16_t m_filled = 0u;     // bytes of the frame received so far
	uint16_t m_remaining = 0u;  // bytes still expected (Body) or to skip (Skip)
	uint8_t m_need = static_cast<uint8_t>(Layout::adu_prefix_size);
	Stage m_stage = Stage::Prefix;
	std::array<uint8_t, prefix_capacity> m_prefix{};
	modbus::rtu::FramingStats m_framing{};
};

} // namespace modbus::rtu::detail

#endif /* MODBUS_RTU_DETAIL_STREAM_RECEIVER_H_ */
