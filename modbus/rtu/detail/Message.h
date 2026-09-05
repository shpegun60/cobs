/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Exclusive, growable owner of one Modbus RTU transmit ADU. */

#ifndef MODBUS_RTU_DETAIL_MESSAGE_H_
#define MODBUS_RTU_DETAIL_MESSAGE_H_

#include "../Crc.h"
#include "../Storage.h"
#include "../../../wire/Scalar.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace modbus::rtu {

template<class StorageT, class CrcT>
class Endpoint;

template<class StorageT>
class Message final {
	static_assert(modbus::rtu::Storage<StorageT>,
		"Message storage must satisfy modbus::rtu::Storage");
	static_assert(StorageT::max_adu_size == modbus::rtu::max_adu_size &&
	              StorageT::max_data_size == modbus::max_data_size,
		"Modbus RTU storage must provide the complete fixed RTU geometry");
	template<class, class>
	friend class Endpoint;

public:
	static constexpr std::size_t max_payload_size = modbus::max_data_size;

	Message() noexcept = default;

	Message(StorageT& storage,
	        const uint8_t address,
	        const uint8_t function,
	        const std::size_t hint) noexcept
		: m_storage(&storage), m_address(address), m_function(function)
	{
		if (hint > max_payload_size) {
			return;
		}
		m_block = storage.acquire_tx(hint);
		if (m_block.memory != nullptr) {
			write_header();
			m_state = State::Building;
		}
	}

	~Message() { release(); }
	Message(const Message&) = delete;
	Message& operator=(const Message&) = delete;

	Message(Message&& other) noexcept
		: m_storage(other.m_storage), m_block(other.m_block),
		  m_size(other.m_size), m_wire(other.m_wire),
		  m_address(other.m_address), m_function(other.m_function),
		  m_state(other.m_state)
	{
		other.disown();
	}

	Message& operator=(Message&& other) noexcept
	{
		if (this != &other) {
			release();
			m_storage = other.m_storage;
			m_block = other.m_block;
			m_size = other.m_size;
			m_wire = other.m_wire;
			m_address = other.m_address;
			m_function = other.m_function;
			m_state = other.m_state;
			other.disown();
		}
		return *this;
	}

	[[nodiscard]] explicit operator bool() const noexcept
	{
		return m_block.memory != nullptr;
	}

	[[nodiscard]] std::size_t size() const noexcept { return m_size; }
	[[nodiscard]] std::size_t capacity() const noexcept { return m_block.capacity; }
	[[nodiscard]] uint8_t address() const noexcept { return m_address; }
	[[nodiscard]] uint8_t function() const noexcept { return m_function; }

	/*
	 * The same scalar vocabulary as cobs::Message. These operations append only
	 * function data; address/function are constructor metadata and finalize()
	 * owns the low-byte-first CRC. Every failure leaves size, capacity and all
	 * existing bytes unchanged.
	 */
	template<wire::Scalar T>
	[[nodiscard]] bool append_native(const T& value) noexcept
	{
		if (!make_room(sizeof(T))) {
			return false;
		}
		wire::detail::store_native(data_ptr() + m_size, value);
		m_size += sizeof(T);
		return true;
	}

	template<wire::EndianScalar T>
	[[nodiscard]] bool append_be(const T& value) noexcept
	{
		return append_ordered<std::endian::big>(value);
	}

	template<wire::EndianScalar T>
	[[nodiscard]] bool append_le(const T& value) noexcept
	{
		return append_ordered<std::endian::little>(value);
	}

	[[nodiscard]] bool append_bytes(
			const std::span<const uint8_t> bytes) noexcept
	{
		return append(bytes.data(), bytes.size());
	}

	template<wire::Scalar T>
	[[nodiscard]] bool append_native(
			const std::span<const T> values) noexcept
	{
		if (values.size() > max_payload_size / sizeof(T)) {
			return false;
		}
		return append(reinterpret_cast<const uint8_t*>(values.data()),
		              values.size() * sizeof(T));
	}

	template<wire::EndianScalar T>
	[[nodiscard]] bool append_be(
			const std::span<const T> values) noexcept
	{
		return append_ordered<std::endian::big>(values);
	}

	template<wire::EndianScalar T>
	[[nodiscard]] bool append_le(
			const std::span<const T> values) noexcept
	{
		return append_ordered<std::endian::little>(values);
	}

	[[nodiscard]] bool reserve(const std::size_t required) noexcept
	{
		if (m_state != State::Building) {
			return false;
		}
		if (required <= m_block.capacity) {
			return true;
		}
		if (required > max_payload_size) {
			return false;
		}

		const TxBlock fresh = m_storage->acquire_tx(
			grow_target(m_block.capacity, required));
		if (fresh.memory == nullptr) {
			return false;
		}
		std::memcpy(fresh.memory, m_block.memory, adu_prefix_size + m_size);
		m_storage->release_tx(m_block);
		m_block = fresh;
		return true;
	}

private:
	enum class State : uint8_t { Empty, Building, Finalized };

	[[nodiscard]] bool belongs_to(const StorageT& storage) const noexcept
	{
		return m_storage == &storage;
	}

	template<modbus::rtu::crc::Calculator CrcT>
	[[nodiscard]] std::span<const uint8_t> finalize(
			CrcT& calculator) noexcept
	{
		if (m_block.memory == nullptr) {
			return {};
		}
		uint8_t* const bytes = raw();
		if (m_state == State::Finalized) {
			return {bytes, m_wire};
		}
		const std::size_t without_crc = adu_prefix_size + m_size;
		const uint16_t checksum = modbus::rtu::crc::calculate(
			std::span<const uint8_t>{bytes, without_crc}, calculator);
		modbus::rtu::crc::store(bytes + without_crc, checksum);
		m_wire = without_crc + modbus::rtu::crc::wire_size;
		m_state = State::Finalized;
		return {bytes, m_wire};
	}

	[[nodiscard]] TxBlock surrender_block() noexcept
	{
		const TxBlock block = m_block;
		m_block = {};
		disown();
		return block;
	}

	[[nodiscard]] static constexpr std::size_t grow_target(
			const std::size_t capacity,
			const std::size_t required) noexcept
	{
		const std::size_t half = capacity >> 1u;
		const std::size_t delta = half == 0u ? 1u : half;
		const std::size_t headroom = max_payload_size - capacity;
		const std::size_t grown = capacity +
			(delta > headroom ? headroom : delta);
		const std::size_t target = required > grown ? required : grown;
		return target > max_payload_size ? max_payload_size : target;
	}

	[[nodiscard]] bool make_room(const std::size_t count) noexcept
	{
		if (m_state != State::Building || count > max_payload_size - m_size) {
			return false;
		}
		return reserve(m_size + count);
	}

	template<std::endian Order, wire::EndianScalar T>
	[[nodiscard]] bool append_ordered(const T& value) noexcept
	{
		if (!make_room(sizeof(T))) {
			return false;
		}
		wire::detail::store_ordered<Order>(data_ptr() + m_size, value);
		m_size += sizeof(T);
		return true;
	}

	template<std::endian Order, wire::EndianScalar T>
	[[nodiscard]] bool append_ordered(
			const std::span<const T> values) noexcept
	{
		if (values.size() > max_payload_size / sizeof(T)) {
			return false;
		}
		const std::size_t count = values.size() * sizeof(T);
		if (!make_room(count)) {
			return false;
		}
		wire::detail::store_ordered<Order>(data_ptr() + m_size, values);
		m_size += count;
		return true;
	}

	[[nodiscard]] bool append(
			const uint8_t* const source,
			const std::size_t count) noexcept
	{
		if (!make_room(count)) {
			return false;
		}
		if (count != 0u) {
			std::memcpy(data_ptr() + m_size, source, count);
		}
		m_size += count;
		return true;
	}

	void write_header() noexcept
	{
		raw()[0] = m_address;
		raw()[address_size] = m_function;
	}

	[[nodiscard]] uint8_t* raw() const noexcept
	{
		return reinterpret_cast<uint8_t*>(m_block.memory);
	}

	[[nodiscard]] uint8_t* data_ptr() const noexcept
	{
		return raw() + adu_prefix_size;
	}

	void release() noexcept
	{
		if (m_block.memory != nullptr) {
			m_storage->release_tx(m_block);
		}
		disown();
	}

	void disown() noexcept
	{
		m_storage = nullptr;
		m_block = {};
		m_size = 0u;
		m_wire = 0u;
		m_address = 0u;
		m_function = 0u;
		m_state = State::Empty;
	}

	StorageT* m_storage = nullptr;
	TxBlock m_block{};
	std::size_t m_size = 0u;
	std::size_t m_wire = 0u;
	uint8_t m_address = 0u;
	uint8_t m_function = 0u;
	State m_state = State::Empty;
};

} // namespace modbus::rtu

#endif /* MODBUS_RTU_DETAIL_MESSAGE_H_ */
