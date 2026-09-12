/* Author: shpegun60; SPDX-License-Identifier: MIT */
#ifndef MODBUS_TCP_DETAIL_MESSAGE_H_
#define MODBUS_TCP_DETAIL_MESSAGE_H_
#include "../Format.h"
#include "../../../wire/Scalar.h"
#include "../../../wire/Storage.h"
#include <cstring>
#include <span>

namespace modbus::tcp {
template<class, class> class Endpoint;

// Exclusive growable ADU owner. The original storage grant is never rewritten.
template<class StorageT, class LayoutT>
class Message final {
	static_assert(wire::ByteStorage<StorageT>);
	template<class, class> friend class Endpoint;
public:
	using Storage = StorageT;
	using Layout = LayoutT;
	static constexpr std::size_t max_payload_size = Layout::max_data_size;
	Message() noexcept = default;
	Message(StorageT& storage, const uint16_t transaction,
		const uint8_t unit, const uint8_t function_code, const std::size_t hint) noexcept
		: m_storage(&storage)
	{
		if (hint > max_payload_size) { return; }
		const std::size_t requested = Layout::adu_size_for_data(hint);
		m_block = storage.acquire_tx(requested);
		if (!honours(m_block, requested)) {
			if (m_block.memory != nullptr) { storage.release_tx(m_block); }
			m_block = {};
			return;
		}
		wire::detail::store_ordered<std::endian::big>(raw(), transaction);
		std::memset(raw() + 2u, 0, 4u); // protocol ID = 0; Length filled at finalize
		raw()[6] = unit;
		raw()[7] = function_code;
		m_state = State::Building;
	}
	~Message() { release(); }
	Message(const Message&) = delete;
	Message& operator=(const Message&) = delete;
	Message(Message&& other) noexcept
		: m_storage(other.m_storage), m_block(other.m_block), m_size(other.m_size),
		  m_state(other.m_state) { other.disown(); }
	Message& operator=(Message&& other) noexcept
	{
		if (this != &other) {
			release();
			m_storage = other.m_storage;
			m_block = other.m_block;
			m_size = other.m_size;
			m_state = other.m_state;
			other.disown();
		}
		return *this;
	}
	[[nodiscard]] explicit operator bool() const noexcept { return m_block.memory != nullptr; }
	[[nodiscard]] std::size_t size() const noexcept { return m_size; }
	[[nodiscard]] std::size_t capacity() const noexcept
	{
		return Layout::data_capacity_for_adu(m_block.granted < Layout::max_adu_size
			? m_block.granted : Layout::max_adu_size);
	}
	[[nodiscard]] uint16_t transaction_id() const noexcept
	{
		return m_block.memory != nullptr
			? wire::detail::load_ordered<std::endian::big, uint16_t>(raw()) : 0u;
	}
	[[nodiscard]] uint8_t unit_id() const noexcept { return *this ? raw()[6] : 0u; }
	[[nodiscard]] uint8_t function() const noexcept { return *this ? raw()[7] : 0u; }

	template<wire::Scalar T>
	[[nodiscard]] bool append_native(const T& value) noexcept
	{
		if (!make_room(sizeof(T))) { return false; }
		wire::detail::store_native(data_ptr() + m_size, value);
		m_size += sizeof(T);
		return true;
	}
	template<wire::EndianScalar T>
	[[nodiscard]] bool append_be(const T& value) noexcept { return append_ordered<std::endian::big>(value); }
	template<wire::EndianScalar T>
	[[nodiscard]] bool append_le(const T& value) noexcept { return append_ordered<std::endian::little>(value); }
	[[nodiscard]] bool append_bytes(const std::span<const uint8_t> bytes) noexcept
	{
		return append(bytes.data(), bytes.size());
	}
	template<wire::Scalar T>
	[[nodiscard]] bool append_native(const std::span<const T> values) noexcept
	{
		if (values.size() > max_payload_size / sizeof(T)) { return false; }
		return append(reinterpret_cast<const uint8_t*>(values.data()), values.size() * sizeof(T));
	}
	template<wire::EndianScalar T>
	[[nodiscard]] bool append_be(const std::span<const T> values) noexcept { return append_ordered<std::endian::big>(values); }
	template<wire::EndianScalar T>
	[[nodiscard]] bool append_le(const std::span<const T> values) noexcept { return append_ordered<std::endian::little>(values); }

	[[nodiscard]] bool reserve(const std::size_t required) noexcept
	{
		if (m_state != State::Building) { return false; }
		if (required <= capacity()) { return true; }
		if (required > max_payload_size) { return false; }
		const std::size_t current = capacity();
		const std::size_t delta = current < 2u ? 1u : current / 2u;
		const std::size_t headroom = max_payload_size - current;
		const std::size_t grown = current + (delta < headroom ? delta : headroom);
		const std::size_t target = required > grown ? required : grown;
		const std::size_t requested = Layout::adu_size_for_data(target);
		const wire::TxBlock fresh = m_storage->acquire_tx(requested);
		if (!honours(fresh, requested)) {
			if (fresh.memory != nullptr) { m_storage->release_tx(fresh); }
			return false;
		}
		std::memcpy(fresh.memory, m_block.memory, Layout::adu_prefix_size + m_size);
		m_storage->release_tx(m_block);
		m_block = fresh;
		return true;
	}
private:
	enum class State : uint8_t { Empty, Building, Finalized };
	[[nodiscard]] bool belongs_to(const StorageT& storage) const noexcept { return m_storage == &storage; }
	template<::crc::Policy CrcT> requires (CrcT::wire_size == Layout::crc_size)
	[[nodiscard]] std::span<const uint8_t> finalize(CrcT& policy) noexcept
	{
		if (!*this) { return {}; }
		const std::size_t total = Layout::adu_size_for_data(m_size);
		if (m_state != State::Finalized) {
			// Length includes the optional private trailer; CRC covers final MBAP.
			wire::detail::store_ordered<std::endian::big>(raw() + 4u,
				static_cast<uint16_t>(total - Layout::length_prefix_size));
			const std::size_t body_size = total - Layout::crc_size;
			const typename CrcT::value_type checksum = policy.calculate({raw(), body_size});
			policy.store(raw() + body_size, checksum);
			m_state = State::Finalized;
		}
		return {raw(), total};
	}
	[[nodiscard]] wire::TxBlock surrender_block() noexcept
	{
		const wire::TxBlock block = m_block;
		disown();
		return block;
	}
	[[nodiscard]] static bool honours(const wire::TxBlock block, const std::size_t bytes) noexcept
	{
		return block.memory != nullptr && block.granted >= bytes;
	}
	[[nodiscard]] bool make_room(const std::size_t count) noexcept
	{
		return m_state == State::Building && count <= max_payload_size - m_size && reserve(m_size + count);
	}
	template<std::endian Order, wire::EndianScalar T>
	[[nodiscard]] bool append_ordered(const T& value) noexcept
	{
		if (!make_room(sizeof(T))) { return false; }
		wire::detail::store_ordered<Order>(data_ptr() + m_size, value);
		m_size += sizeof(T);
		return true;
	}
	template<std::endian Order, wire::EndianScalar T>
	[[nodiscard]] bool append_ordered(const std::span<const T> values) noexcept
	{
		if (values.size() > max_payload_size / sizeof(T)) { return false; }
		const std::size_t count = values.size() * sizeof(T);
		if (!make_room(count)) { return false; }
		wire::detail::store_ordered<Order>(data_ptr() + m_size, values);
		m_size += count;
		return true;
	}
	[[nodiscard]] bool append(const uint8_t* const source, const std::size_t count) noexcept
	{
		if (!make_room(count)) { return false; }
		if (count != 0u) { std::memcpy(data_ptr() + m_size, source, count); }
		m_size += count;
		return true;
	}
	[[nodiscard]] uint8_t* raw() const noexcept { return reinterpret_cast<uint8_t*>(m_block.memory); }
	[[nodiscard]] uint8_t* data_ptr() const noexcept { return raw() + Layout::adu_prefix_size; }
	void release() noexcept
	{
		if (m_block.memory != nullptr) { m_storage->release_tx(m_block); }
		disown();
	}
	void disown() noexcept
	{
		m_storage = nullptr;
		m_block = {};
		m_size = 0u;
		m_state = State::Empty;
	}
	StorageT* m_storage = nullptr;
	wire::TxBlock m_block{};
	std::size_t m_size = 0u;
	State m_state = State::Empty;
};
} // namespace modbus::tcp
#endif
