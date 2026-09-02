/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Memory policies for Modbus RTU ownership.
 *
 * RX stores the complete immutable ADU so Packet can expose adu(), pdu() and
 * data() as zero-copy subviews. TX capacity is expressed only in function
 * data bytes; address, function and CRC storage are service bytes owned by
 * the library and never reduce the advertised capacity.
 */

#ifndef MODBUS_RTU_STORAGE_H_
#define MODBUS_RTU_STORAGE_H_

#include "Crc.h"
#include "../Types.h"
#include "../detail/BlockPool.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <span>

namespace modbus::rtu {

inline constexpr std::size_t address_size = 1u;
inline constexpr std::size_t function_size = 1u;
inline constexpr std::size_t crc_size = modbus::rtu::crc::wire_size;
inline constexpr std::size_t adu_prefix_size = address_size + function_size;
inline constexpr std::size_t adu_overhead = adu_prefix_size + crc_size;
inline constexpr std::size_t pdu_envelope_size = address_size + crc_size;
inline constexpr std::size_t min_adu_size = adu_overhead;
inline constexpr std::size_t max_adu_size =
	address_size + modbus::max_pdu_size + crc_size;

static_assert(min_adu_size == 4u && max_adu_size == 256u);

template<class StorageT>
class Packet;

namespace detail {

template<class StorageT>
class Receiver;

} // namespace detail

struct TxBlock final {
	std::byte* memory = nullptr;
	std::size_t capacity = 0; // function-data bytes
};

template<class StorageT>
struct RxBlock final {
	friend class Packet<StorageT>;
	friend class detail::Receiver<StorageT>;

private:
	uint32_t refs = 1;
	uint16_t adu_size = 0;
	uint8_t address = 0;
	uint8_t function = 0;
	RxBlock* next_ready = nullptr;
	StorageT* owner = nullptr;

	[[nodiscard]] std::span<uint8_t> writable_adu(
			const std::size_t allocated) noexcept
	{
		return {payload(), allocated};
	}

	[[nodiscard]] uint8_t* payload() noexcept
	{
		return reinterpret_cast<uint8_t*>(this) + sizeof(RxBlock);
	}

	[[nodiscard]] const uint8_t* payload() const noexcept
	{
		return reinterpret_cast<const uint8_t*>(this) + sizeof(RxBlock);
	}
};

template<class T>
concept Storage = requires(
	T& storage,
	const std::size_t size,
	typename T::RxBlock* rx,
	TxBlock tx)
{
	typename T::RxBlock;
	requires std::same_as<typename T::RxBlock, modbus::rtu::RxBlock<T>>;

	{ T::max_adu_size } -> std::convertible_to<std::size_t>;
	{ T::max_data_size } -> std::convertible_to<std::size_t>;

	{ storage.acquire_rx(size) }
		noexcept -> std::same_as<typename T::RxBlock*>;
	{ storage.release_rx(rx) }
		noexcept -> std::same_as<void>;
	{ storage.acquire_tx(size) }
		noexcept -> std::same_as<TxBlock>;
	{ storage.release_tx(tx) }
		noexcept -> std::same_as<void>;
};

class Heap final {
public:
	using RxBlock = modbus::rtu::RxBlock<Heap>;
	static constexpr std::size_t max_adu_size = modbus::rtu::max_adu_size;
	static constexpr std::size_t max_data_size = modbus::max_data_size;

	Heap() noexcept = default;
	Heap(const Heap&) = delete;
	Heap& operator=(const Heap&) = delete;

	[[nodiscard]] RxBlock* acquire_rx(const std::size_t requested) noexcept
	{
		if (requested > max_adu_size) {
			return nullptr;
		}
		void* const memory =
			::operator new(sizeof(RxBlock) + requested, std::nothrow);
		return memory != nullptr
			? std::construct_at(static_cast<RxBlock*>(memory))
			: nullptr;
	}

	void release_rx(RxBlock* const block) noexcept
	{
		if (block != nullptr) {
			std::destroy_at(block);
			::operator delete(static_cast<void*>(block));
		}
	}

	[[nodiscard]] TxBlock acquire_tx(const std::size_t requested) noexcept
	{
		if (requested > max_data_size) {
			return {};
		}
		void* const memory = ::operator new(requested + adu_overhead, std::nothrow);
		return memory != nullptr
			? TxBlock{static_cast<std::byte*>(memory), requested}
			: TxBlock{};
	}

	void release_tx(const TxBlock block) noexcept
	{
		::operator delete(static_cast<void*>(block.memory));
	}
};

template<std::size_t RxBlocks, std::size_t TxBlocks>
class Pool final {
public:
	using RxBlock = modbus::rtu::RxBlock<Pool>;
	static constexpr std::size_t max_adu_size = modbus::rtu::max_adu_size;
	static constexpr std::size_t max_data_size = modbus::max_data_size;
	static constexpr std::size_t rx_blocks = RxBlocks;
	static constexpr std::size_t tx_blocks = TxBlocks;

private:
	using RxPool = modbus::detail::BlockPool<
		sizeof(RxBlock) + max_adu_size, RxBlocks, alignof(RxBlock)>;
	using TxPool = modbus::detail::BlockPool<
		max_adu_size, TxBlocks, 1u>;

public:
	using PoolStats = modbus::detail::PoolStats;

	Pool() noexcept = default;
	Pool(const Pool&) = delete;
	Pool& operator=(const Pool&) = delete;

	[[nodiscard]] RxBlock* acquire_rx(const std::size_t requested) noexcept
	{
		if (requested > max_adu_size) {
			return nullptr;
		}
		std::byte* const memory = m_rx.acquire();
		return memory != nullptr
			? std::construct_at(
				static_cast<RxBlock*>(static_cast<void*>(memory)))
			: nullptr;
	}

	void release_rx(RxBlock* const block) noexcept
	{
		(void)m_rx.release(
			static_cast<std::byte*>(static_cast<void*>(block)),
			[](std::byte* const memory) noexcept {
				std::destroy_at(
					static_cast<RxBlock*>(static_cast<void*>(memory)));
			});
	}

	[[nodiscard]] TxBlock acquire_tx(const std::size_t requested) noexcept
	{
		if (requested > max_data_size) {
			return {};
		}
		std::byte* const memory = m_tx.acquire();
		return memory != nullptr
			? TxBlock{memory, max_data_size}
			: TxBlock{};
	}

	void release_tx(const TxBlock block) noexcept
	{
		m_tx.release(block.memory);
	}

	[[nodiscard]] std::size_t rx_available() const noexcept { return m_rx.available(); }
	[[nodiscard]] std::size_t tx_available() const noexcept { return m_tx.available(); }
	[[nodiscard]] const PoolStats& rx_stats() const noexcept { return m_rx.stats(); }
	[[nodiscard]] const PoolStats& tx_stats() const noexcept { return m_tx.stats(); }

private:
	RxPool m_rx{};
	TxPool m_tx{};
};

static_assert(Storage<Heap>);
static_assert(Storage<Pool<1, 1>>);

} // namespace modbus::rtu

#endif /* MODBUS_RTU_STORAGE_H_ */
