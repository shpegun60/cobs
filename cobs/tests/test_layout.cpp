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

using Heap = cobs::Heap<cobs::Format<1024, 1024>>;
using Pool = cobs::Pool<cobs::Format<1024, 1024>, 8, 2>;
using Block = cobs::RxBlock<Heap>;
using Packet = cobs::Packet<Heap>;
using Message = cobs::Message<Heap>;
using Decoder = cobs::codec::Decoder;
using Receiver = cobs::detail::Receiver<Heap>;
using Endpoint = cobs::Endpoint<Heap>;
using PoolEndpoint = cobs::Endpoint<Pool>;
using Sender = typename Endpoint::Sender;
using BusyQuery = typename Endpoint::TxBusy;
using RxStats = typename Receiver::Stats;
using TxStats = typename Endpoint::TxStats;
using PoolStats = cobs::detail::PoolStats;

static_assert(sizeof(Packet) == sizeof(void*),
	"a cobs::Packet is exactly one typed pointer");
static_assert(sizeof(cobs::TxBlock) == sizeof(std::byte*) + sizeof(std::size_t),
	"TxBlock is exactly one pointer plus its reported capacity");
static_assert(sizeof(Heap) == 1,
	"the stateless heap policy must remain eligible for no_unique_address");

#define COBS_EXPECT_SIZE(Type, Expected) \
	static_assert(sizeof(Type) == (Expected), #Type " layout changed; review ownership and padding")

#if INTPTR_MAX == INT64_MAX
COBS_EXPECT_SIZE(cobs::TxBlock, 16);
COBS_EXPECT_SIZE(Block, 24);
COBS_EXPECT_SIZE(Packet, 8);
COBS_EXPECT_SIZE(Message, 48);
COBS_EXPECT_SIZE(Decoder, 48);
COBS_EXPECT_SIZE(Receiver, 136);
COBS_EXPECT_SIZE(Endpoint, 304);
COBS_EXPECT_SIZE(Heap, 1);
COBS_EXPECT_SIZE(Pool, 10496);
COBS_EXPECT_SIZE(PoolEndpoint, 10800);
COBS_EXPECT_SIZE(Sender, 64);
COBS_EXPECT_SIZE(BusyQuery, 64);
COBS_EXPECT_SIZE(RxStats, 28);
COBS_EXPECT_SIZE(TxStats, 12);
COBS_EXPECT_SIZE(PoolStats, 16);
#elif defined(__arm__) && INTPTR_MAX == INT32_MAX
COBS_EXPECT_SIZE(cobs::TxBlock, 8);
COBS_EXPECT_SIZE(Block, 16);
COBS_EXPECT_SIZE(Packet, 4);
COBS_EXPECT_SIZE(Message, 24);
COBS_EXPECT_SIZE(Decoder, 24);
COBS_EXPECT_SIZE(Receiver, 80);
COBS_EXPECT_SIZE(Endpoint, 168);
COBS_EXPECT_SIZE(Heap, 1);
COBS_EXPECT_SIZE(Pool, 10424);
COBS_EXPECT_SIZE(PoolEndpoint, 10592);
COBS_EXPECT_SIZE(Sender, 32);
COBS_EXPECT_SIZE(BusyQuery, 32);
COBS_EXPECT_SIZE(RxStats, 28);
COBS_EXPECT_SIZE(TxStats, 12);
COBS_EXPECT_SIZE(PoolStats, 16);
#endif

#undef COBS_EXPECT_SIZE

} // namespace

// These C-linkage arrays encode every measured size in their symbol size.
// Keep them externally visible: cobs/tests/check_arm_layout.sh reads them from
// a Cortex-M object using arm-none-eabi-nm.
extern "C" {
[[maybe_unused]] std::byte cobs_layout_pointer[sizeof(void*)]{};
[[maybe_unused]] std::byte cobs_layout_size_t[sizeof(std::size_t)]{};
[[maybe_unused]] std::byte cobs_layout_tx_block[sizeof(cobs::TxBlock)]{};
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
[[maybe_unused]] std::byte cobs_layout_pool_stats[sizeof(PoolStats)]{};
}

int main()
{
	std::printf("\n[Layout]\n");
	std::printf("pointer=%zu size_t=%zu cobs::TxBlock=%zu cobs::RxBlock=%zu cobs::Packet=%zu\n",
		sizeof(void*), sizeof(std::size_t), sizeof(cobs::TxBlock), sizeof(Block), sizeof(Packet));
	std::printf("cobs::Message=%zu Decoder=%zu Receiver=%zu Endpoint=%zu\n",
		sizeof(Message), sizeof(Decoder), sizeof(Receiver), sizeof(Endpoint));
	std::printf("Heap=%zu Pool=%zu PoolEndpoint=%zu sender=%zu busy=%zu\n",
		sizeof(Heap), sizeof(Pool), sizeof(PoolEndpoint), sizeof(Sender), sizeof(BusyQuery));
	std::printf("RxStats=%zu TxStats=%zu PoolStats=%zu\n",
		sizeof(RxStats), sizeof(TxStats), sizeof(PoolStats));
	return 0;
}
