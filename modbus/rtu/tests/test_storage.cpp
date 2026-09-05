/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Format.h"
#include "modbus/rtu/Storage.h"
#include "Test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

using namespace modbus_test;

namespace {

struct MissingTx {
	using RxBlock = modbus::rtu::RxBlock<MissingTx>;
	static constexpr std::size_t max_adu_size = 256u;
	RxBlock* acquire_rx(std::size_t) noexcept;
	void release_rx(RxBlock*) noexcept;
};

struct ThrowingRx {
	using RxBlock = modbus::rtu::RxBlock<ThrowingRx>;
	static constexpr std::size_t max_adu_size = 256u;
	RxBlock* acquire_rx(std::size_t);
	void release_rx(RxBlock*) noexcept;
	modbus::rtu::TxBlock acquire_tx(std::size_t) noexcept;
	void release_tx(modbus::rtu::TxBlock) noexcept;
};

static_assert(modbus::rtu::Storage<modbus::rtu::Heap>);
static_assert(modbus::rtu::Storage<modbus::rtu::Pool<2, 1>>);
static_assert(!modbus::rtu::Storage<MissingTx>);
static_assert(!modbus::rtu::Storage<ThrowingRx>);
using DefaultFormat = modbus::rtu::DefaultFormat;
static_assert(DefaultFormat::address_size == 1u);
static_assert(DefaultFormat::function_size == 1u);
static_assert(DefaultFormat::crc_size ==
	modbus::rtu::crc::Bitwise::wire_size);
static_assert(DefaultFormat::adu_prefix_size == 2u);
static_assert(DefaultFormat::pdu_envelope_size == 3u);
static_assert(DefaultFormat::adu_overhead == 4u);

template<class Storage>
void contract(const char* const name)
{
	group(name);
	Storage storage;
	using Block = typename Storage::RxBlock;

	Block* const rx = storage.acquire_rx(modbus::rtu::max_adu_size);
	check(rx != nullptr, "maximum RTU RX ADU allocation succeeds");
	check(storage.acquire_rx(modbus::rtu::max_adu_size + 1u) == nullptr,
	      "oversize RX allocation is refused");
	storage.release_rx(rx);
	storage.release_rx(nullptr);

	constexpr std::array<std::size_t, 4> requests{
		1u, 2u, 34u, modbus::rtu::max_adu_size};
	for (const std::size_t request : requests) {
		const modbus::rtu::TxBlock tx = storage.acquire_tx(request);
		check(tx.memory != nullptr && tx.adu_capacity >= request &&
		      tx.adu_capacity <= modbus::rtu::max_adu_size,
		      "TX reports a valid complete-ADU capacity");
		if (tx.memory != nullptr) {
			for (std::size_t i = 0; i < tx.adu_capacity; ++i) {
				tx.memory[i] = static_cast<std::byte>(i);
			}
		}
		storage.release_tx(tx);
	}
	check(storage.acquire_tx(modbus::rtu::max_adu_size + 1u).memory == nullptr,
	      "oversize TX allocation is refused");
}

} // namespace

int main()
{
	contract<modbus::rtu::Heap>("HeapContract");
	contract<modbus::rtu::Pool<2, 2>>("PoolContract");

	group("PoolQuotas");
	using Pool = modbus::rtu::Pool<2, 1>;
	Pool pool;
	auto* const a = pool.acquire_rx(4u);
	auto* const b = pool.acquire_rx(256u);
	check(a != nullptr && b != nullptr && a != b && pool.rx_available() == 0u,
	      "RX blocks are distinct and exhaust independently");
	check(pool.acquire_rx(4u) == nullptr && pool.rx_stats().exhausted == 1u,
	      "dry RX pool reports backpressure");
	const auto tx = pool.acquire_tx(1u);
	check(tx.memory != nullptr && pool.tx_available() == 0u,
	      "TX remains available while RX is exhausted");
	check(pool.acquire_tx(1u).memory == nullptr && pool.tx_stats().exhausted == 1u,
	      "TX exhaustion is counted separately");
	pool.release_rx(a);
	pool.release_rx(a);
	check(pool.rx_stats().rejected == 1u && pool.rx_available() == 1u,
	      "double release is rejected without corrupting the pool");
	pool.release_rx(b);
	pool.release_tx(tx);
	check(pool.rx_available() == 2u && pool.tx_available() == 1u,
	      "all valid blocks return to their independent pools");

	group("Layout");
	check(std::is_trivially_copyable_v<modbus::rtu::TxBlock>,
	      "TX ownership descriptor is a trivial pointer-capacity pair");
	check(modbus::rtu::Pool<1, 1>::max_adu_size == 256u,
	      "storage republishes only the physical RTU ADU limit");

	return finish();
}
