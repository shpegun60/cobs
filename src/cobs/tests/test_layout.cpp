/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Layout characterization for the ownership-bearing COBS types.
 *
 * Exact ABI snapshots are intentionally kept here, next to the executable
 * proof, rather than only in prose. The symbols below also make a compile-only
 * ARM object inspectable with arm-none-eabi-nm; no target execution is needed.
 */

#include "Cobs.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

using Format = cobs::Format<crc::NoCrc, 1024>;
using Endpoint = cobs::Endpoint<wire::Heap, Format>;
using PoolEndpoint = cobs::Endpoint<wire::Pool<8, 2>, Format>;
using Heap = Endpoint::Storage;
using Pool = PoolEndpoint::Storage;
using Block = cobs::RxBlock<Heap>;
using Packet = Endpoint::Packet;
using Message = Endpoint::Message;
using Decoder = cobs::codec::Decoder;
using Receiver = cobs::detail::Receiver<Heap, Format::Layout>;
using Sender = typename Endpoint::Sender;
using BusyQuery = typename Endpoint::BusyQuery;
using RxStats = cobs::Stats::Rx;
using TxStats = cobs::Stats::Tx;
using Stats = cobs::Stats;
using PoolStats = wire::detail::PoolStats;
using DefaultEndpoint = cobs::Endpoint<>;
using DefaultPoolEndpoint = cobs::Endpoint<wire::Pool<8, 2>>;
static_assert(DefaultEndpoint::crc_size == 2u && DefaultEndpoint::max_send_size == 253u);
static_assert(DefaultEndpoint::Geometry::tx_block_bytes == 259u);
static_assert(sizeof(DefaultEndpoint::Packet) == sizeof(void*));

static_assert(sizeof(Packet) == sizeof(void*),
	"a cobs::Packet is exactly one typed pointer");
static_assert(sizeof(wire::TxBlock) == sizeof(std::byte*) + sizeof(std::size_t),
	"TxBlock is exactly one pointer plus its reported capacity");
static_assert(sizeof(Heap) == 1,
	"the stateless heap policy must remain eligible for no_unique_address");

#define COBS_EXPECT_SIZE(Type, Expected) \
	static_assert(sizeof(Type) == (Expected), #Type " layout changed; review ownership and padding")

#if !defined(COBS_LAYOUT_CHARACTERIZE)
#if defined(_MSC_VER)
// MSVC snapshots are intentionally separate from the Itanium C++ ABI used by
// GCC/Clang: tiny_delegate has a different, but stable, member-function pointer
// representation there.
#if INTPTR_MAX == INT64_MAX
COBS_EXPECT_SIZE(wire::TxBlock, 16);
COBS_EXPECT_SIZE(Block, 24);
COBS_EXPECT_SIZE(Packet, 8);
COBS_EXPECT_SIZE(Message, 56);
COBS_EXPECT_SIZE(Decoder, 48);
COBS_EXPECT_SIZE(Receiver, 120);
COBS_EXPECT_SIZE(Endpoint, 272);
COBS_EXPECT_SIZE(Heap, 1);
COBS_EXPECT_SIZE(Pool, 10496);
COBS_EXPECT_SIZE(PoolEndpoint, 10768);
COBS_EXPECT_SIZE(Sender, 56);
COBS_EXPECT_SIZE(BusyQuery, 56);
COBS_EXPECT_SIZE(RxStats, 32);
COBS_EXPECT_SIZE(TxStats, 12);
COBS_EXPECT_SIZE(Stats, 44);
COBS_EXPECT_SIZE(PoolStats, 16);
#elif INTPTR_MAX == INT32_MAX
COBS_EXPECT_SIZE(wire::TxBlock, 8);
COBS_EXPECT_SIZE(Block, 16);
COBS_EXPECT_SIZE(Packet, 4);
COBS_EXPECT_SIZE(Message, 28);
COBS_EXPECT_SIZE(Decoder, 24);
COBS_EXPECT_SIZE(Receiver, 76);
COBS_EXPECT_SIZE(Endpoint, 168);
COBS_EXPECT_SIZE(Heap, 1);
COBS_EXPECT_SIZE(Pool, 10424);
COBS_EXPECT_SIZE(PoolEndpoint, 10592);
COBS_EXPECT_SIZE(Sender, 32);
COBS_EXPECT_SIZE(BusyQuery, 32);
COBS_EXPECT_SIZE(RxStats, 32);
COBS_EXPECT_SIZE(TxStats, 12);
COBS_EXPECT_SIZE(Stats, 44);
COBS_EXPECT_SIZE(PoolStats, 16);
#endif
#elif INTPTR_MAX == INT64_MAX
COBS_EXPECT_SIZE(wire::TxBlock, 16);
COBS_EXPECT_SIZE(Block, 24);
COBS_EXPECT_SIZE(Packet, 8);
COBS_EXPECT_SIZE(Message, 56);
COBS_EXPECT_SIZE(Decoder, 48);
COBS_EXPECT_SIZE(Receiver, 120);
COBS_EXPECT_SIZE(Endpoint, 288);
COBS_EXPECT_SIZE(Heap, 1);
COBS_EXPECT_SIZE(Pool, 10496);
COBS_EXPECT_SIZE(PoolEndpoint, 10784);
COBS_EXPECT_SIZE(Sender, 64);
COBS_EXPECT_SIZE(BusyQuery, 64);
COBS_EXPECT_SIZE(RxStats, 32);
COBS_EXPECT_SIZE(TxStats, 12);
COBS_EXPECT_SIZE(Stats, 44);
COBS_EXPECT_SIZE(PoolStats, 16);
#elif defined(__arm__) && INTPTR_MAX == INT32_MAX
COBS_EXPECT_SIZE(wire::TxBlock, 8);
COBS_EXPECT_SIZE(Block, 16);
COBS_EXPECT_SIZE(Packet, 4);
COBS_EXPECT_SIZE(Message, 28);
COBS_EXPECT_SIZE(Decoder, 24);
COBS_EXPECT_SIZE(Receiver, 76);
COBS_EXPECT_SIZE(Endpoint, 168);
COBS_EXPECT_SIZE(Heap, 1);
COBS_EXPECT_SIZE(Pool, 10424);
COBS_EXPECT_SIZE(PoolEndpoint, 10592);
COBS_EXPECT_SIZE(Sender, 32);
COBS_EXPECT_SIZE(BusyQuery, 32);
COBS_EXPECT_SIZE(RxStats, 32);
COBS_EXPECT_SIZE(TxStats, 12);
COBS_EXPECT_SIZE(Stats, 44);
COBS_EXPECT_SIZE(PoolStats, 16);
#endif
#endif

#undef COBS_EXPECT_SIZE

} // namespace

// These C-linkage arrays encode every measured size in their symbol size.
// Keep them externally visible: cobs/tests/check_arm_layout.sh reads them from
// a Cortex-M object using arm-none-eabi-nm.
extern "C" {
[[maybe_unused]] std::byte cobs_layout_pointer[sizeof(void*)]{};
[[maybe_unused]] std::byte cobs_layout_size_t[sizeof(std::size_t)]{};
[[maybe_unused]] std::byte cobs_layout_tx_block[sizeof(wire::TxBlock)]{};
[[maybe_unused]] std::byte cobs_layout_rx_block[sizeof(Block)]{};
[[maybe_unused]] std::byte cobs_layout_packet[sizeof(Packet)]{};
[[maybe_unused]] std::byte cobs_layout_message[sizeof(Message)]{};
[[maybe_unused]] std::byte cobs_layout_decoder[sizeof(Decoder)]{};
[[maybe_unused]] std::byte cobs_layout_receiver[sizeof(Receiver)]{};
[[maybe_unused]] std::byte cobs_layout_endpoint[sizeof(Endpoint)]{};
[[maybe_unused]] std::byte cobs_layout_heap[sizeof(Heap)]{};
[[maybe_unused]] std::byte cobs_layout_pool[sizeof(Pool)]{};
[[maybe_unused]] std::byte cobs_layout_pool_endpoint[sizeof(PoolEndpoint)]{};
[[maybe_unused]] std::byte cobs_layout_sender[sizeof(Sender)]{};
[[maybe_unused]] std::byte cobs_layout_busy_query[sizeof(BusyQuery)]{};
[[maybe_unused]] std::byte cobs_layout_rx_stats[sizeof(RxStats)]{};
[[maybe_unused]] std::byte cobs_layout_tx_stats[sizeof(TxStats)]{};
[[maybe_unused]] std::byte cobs_layout_stats[sizeof(Stats)]{};
[[maybe_unused]] std::byte cobs_layout_pool_stats[sizeof(PoolStats)]{};
[[maybe_unused]] std::byte cobs_layout_default_endpoint[sizeof(DefaultEndpoint)]{};
[[maybe_unused]] std::byte cobs_layout_default_pool_endpoint[sizeof(DefaultPoolEndpoint)]{};
}

int main()
{
	std::printf("\n[Layout]\n");
	std::printf("pointer=%zu size_t=%zu wire::TxBlock=%zu cobs::RxBlock=%zu cobs::Packet=%zu\n",
		sizeof(void*), sizeof(std::size_t), sizeof(wire::TxBlock), sizeof(Block), sizeof(Packet));
	std::printf("cobs::Message=%zu Decoder=%zu Receiver=%zu Endpoint=%zu\n",
		sizeof(Message), sizeof(Decoder), sizeof(Receiver), sizeof(Endpoint));
	std::printf("Heap=%zu Pool=%zu PoolEndpoint=%zu sender=%zu busy=%zu\n",
		sizeof(Heap), sizeof(Pool), sizeof(PoolEndpoint), sizeof(Sender), sizeof(BusyQuery));
	std::printf("RxStats=%zu TxStats=%zu Stats=%zu PoolStats=%zu\n",
		sizeof(RxStats), sizeof(TxStats), sizeof(Stats), sizeof(PoolStats));
	std::printf("CRC16/253 default: Endpoint=%zu PoolEndpoint=%zu RX=%zu TX=%zu alignment=%zu\n",
		sizeof(DefaultEndpoint), sizeof(DefaultPoolEndpoint),
		DefaultEndpoint::Geometry::rx_block_bytes, DefaultEndpoint::Geometry::tx_block_bytes,
		DefaultEndpoint::Geometry::alignment);
	return 0;
}
