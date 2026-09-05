/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"

#include <cstddef>
#include <cstdio>
#include <type_traits>

namespace {

using HeapEndpoint = modbus::rtu::Endpoint<>;
using Memory = wire::Pool<8, 2>;
using PoolEndpoint = modbus::rtu::Endpoint<Memory>;
using Pool = PoolEndpoint::Storage;
using TableHeapEndpoint = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<::crc::Crc16Table>>;
using TablePoolEndpoint = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<::crc::Crc16Table>>;
using NoCrcHeapEndpoint = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<::crc::NoCrc>>;
using Crc64TablePoolEndpoint = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<::crc::Crc64Table>>;

struct StatefulCrc final : ::crc::Codec<uint16_t, 2u, std::endian::little> {
	void* handle = nullptr;

	[[nodiscard]] uint16_t calculate(
			const std::span<const uint8_t> bytes) noexcept
	{
		return ::crc::calculate<::crc::Crc16Bitwise>(bytes);
	}
};

using StatefulHeapEndpoint = modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<StatefulCrc>>;
using StatefulPoolEndpoint = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<StatefulCrc>>;
using Packet = HeapEndpoint::Packet;
using Message = HeapEndpoint::Message;
using Sender = HeapEndpoint::Sender;
using BusyQuery = HeapEndpoint::BusyQuery;

static_assert(sizeof(Packet) == sizeof(void*));
static_assert(std::is_copy_constructible_v<Packet>);
static_assert(!std::is_copy_constructible_v<Message>);
static_assert(!std::is_move_constructible_v<HeapEndpoint>);
static_assert(std::is_empty_v<::crc::Crc16Bitwise>);
static_assert(std::is_empty_v<::crc::Crc16Table>);
static_assert(sizeof(TableHeapEndpoint) == sizeof(HeapEndpoint));
static_assert(sizeof(TablePoolEndpoint) == sizeof(PoolEndpoint));
static_assert(sizeof(NoCrcHeapEndpoint) == sizeof(HeapEndpoint));
static_assert(sizeof(Crc64TablePoolEndpoint) == sizeof(PoolEndpoint));
static_assert(sizeof(StatefulHeapEndpoint) >= sizeof(HeapEndpoint));
static_assert(sizeof(StatefulHeapEndpoint) <=
	(sizeof(HeapEndpoint) + sizeof(StatefulCrc) +
	 alignof(StatefulHeapEndpoint) - 1u));
static_assert(sizeof(StatefulPoolEndpoint) >= sizeof(PoolEndpoint));
static_assert(sizeof(StatefulPoolEndpoint) <=
	(sizeof(PoolEndpoint) + sizeof(StatefulCrc) +
	 alignof(StatefulPoolEndpoint) - 1u));
static_assert(sizeof(modbus::rtu::Stats) == 40u);
#if !defined(MODBUS_LAYOUT_CHARACTERIZE)
#define MODBUS_EXPECT_SIZE(Type, Expected) \
	static_assert(sizeof(Type) == (Expected), #Type " layout changed; review ownership and padding")

#if defined(_MSC_VER) && INTPTR_MAX == INT64_MAX
MODBUS_EXPECT_SIZE(wire::TxBlock, 16);
MODBUS_EXPECT_SIZE(modbus::rtu::RxBlock<wire::Heap>, 24);
MODBUS_EXPECT_SIZE(Packet, 8);
MODBUS_EXPECT_SIZE(Message, 48);
MODBUS_EXPECT_SIZE(HeapEndpoint, 208);
MODBUS_EXPECT_SIZE(Pool, 2800);
MODBUS_EXPECT_SIZE(PoolEndpoint, 3000);
MODBUS_EXPECT_SIZE(Sender, 56);
MODBUS_EXPECT_SIZE(BusyQuery, 56);
#elif defined(_MSC_VER) && INTPTR_MAX == INT32_MAX
MODBUS_EXPECT_SIZE(wire::TxBlock, 8);
MODBUS_EXPECT_SIZE(modbus::rtu::RxBlock<wire::Heap>, 16);
MODBUS_EXPECT_SIZE(Packet, 4);
MODBUS_EXPECT_SIZE(Message, 24);
MODBUS_EXPECT_SIZE(HeapEndpoint, 136);
MODBUS_EXPECT_SIZE(Pool, 2728);
MODBUS_EXPECT_SIZE(PoolEndpoint, 2856);
MODBUS_EXPECT_SIZE(Sender, 32);
MODBUS_EXPECT_SIZE(BusyQuery, 32);
#elif INTPTR_MAX == INT64_MAX
MODBUS_EXPECT_SIZE(wire::TxBlock, 16);
MODBUS_EXPECT_SIZE(modbus::rtu::RxBlock<wire::Heap>, 24);
MODBUS_EXPECT_SIZE(Packet, 8);
MODBUS_EXPECT_SIZE(Message, 48);
MODBUS_EXPECT_SIZE(HeapEndpoint, 224);
MODBUS_EXPECT_SIZE(Pool, 2800);
MODBUS_EXPECT_SIZE(PoolEndpoint, 3024);
MODBUS_EXPECT_SIZE(Sender, 64);
MODBUS_EXPECT_SIZE(BusyQuery, 64);
#elif defined(__arm__) && INTPTR_MAX == INT32_MAX
MODBUS_EXPECT_SIZE(wire::TxBlock, 8);
MODBUS_EXPECT_SIZE(modbus::rtu::RxBlock<wire::Heap>, 16);
MODBUS_EXPECT_SIZE(Packet, 4);
MODBUS_EXPECT_SIZE(Message, 24);
MODBUS_EXPECT_SIZE(HeapEndpoint, 128);
MODBUS_EXPECT_SIZE(TableHeapEndpoint, 128);
MODBUS_EXPECT_SIZE(StatefulHeapEndpoint, 128);
MODBUS_EXPECT_SIZE(Pool, 2728);
MODBUS_EXPECT_SIZE(PoolEndpoint, 2856);
MODBUS_EXPECT_SIZE(TablePoolEndpoint, 2856);
MODBUS_EXPECT_SIZE(StatefulPoolEndpoint, 2856);
MODBUS_EXPECT_SIZE(Sender, 32);
MODBUS_EXPECT_SIZE(BusyQuery, 32);
#endif

#undef MODBUS_EXPECT_SIZE
#endif

} // namespace

extern "C" {
char modbus_layout_pointer[sizeof(void*)];
char modbus_layout_size_t[sizeof(std::size_t)];
char modbus_layout_tx_block[sizeof(wire::TxBlock)];
char modbus_layout_rx_block[sizeof(modbus::rtu::RxBlock<wire::Heap>)];
char modbus_layout_packet[sizeof(Packet)];
char modbus_layout_message[sizeof(Message)];
char modbus_layout_endpoint[sizeof(HeapEndpoint)];
char modbus_layout_table_endpoint[sizeof(TableHeapEndpoint)];
char modbus_layout_stateful_endpoint[sizeof(StatefulHeapEndpoint)];
char modbus_layout_pool[sizeof(Pool)];
char modbus_layout_pool_endpoint[sizeof(PoolEndpoint)];
char modbus_layout_table_pool_endpoint[sizeof(TablePoolEndpoint)];
char modbus_layout_stateful_pool_endpoint[sizeof(StatefulPoolEndpoint)];
char modbus_layout_stats[sizeof(modbus::rtu::Stats)];
char modbus_layout_sender[sizeof(HeapEndpoint::Sender)];
char modbus_layout_busy_query[sizeof(HeapEndpoint::BusyQuery)];
}

int main()
{
	std::printf("\n[Modbus RTU layout]\n");
	std::printf("pointer=%zu size_t=%zu TxBlock=%zu RxBlock=%zu Packet=%zu\n",
		sizeof(void*), sizeof(std::size_t), sizeof(wire::TxBlock),
		sizeof(modbus::rtu::RxBlock<wire::Heap>), sizeof(Packet));
	std::printf("Message=%zu Endpoint=%zu Pool=%zu PoolEndpoint=%zu\n",
		sizeof(Message), sizeof(HeapEndpoint), sizeof(Pool),
		sizeof(PoolEndpoint));
	std::printf("TableEndpoint=%zu TablePoolEndpoint=%zu StatefulEndpoint=%zu "
	            "StatefulPoolEndpoint=%zu\n",
		sizeof(TableHeapEndpoint), sizeof(TablePoolEndpoint),
		sizeof(StatefulHeapEndpoint), sizeof(StatefulPoolEndpoint));
	std::printf("Stats=%zu sender=%zu busy=%zu\n",
		sizeof(modbus::rtu::Stats), sizeof(Sender), sizeof(BusyQuery));
	return 0;
}
