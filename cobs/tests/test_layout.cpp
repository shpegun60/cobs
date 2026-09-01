/*
 * Layout characterization for the ownership-bearing COBS types.
 *
 * Exact ABI snapshots are intentionally kept here, next to the executable
 * proof, rather than only in prose. The symbols below also make a compile-only
 * ARM object inspectable with arm-none-eabi-nm; no target execution is needed.
 */

#include "Cobs.h"
#include "CobsFixedAllocator.h"
#include "CobsHeapAllocator.h"
#include "CobsMsg.h"
#include "CobsRx.h"
#include "PacketRef.h"
#include "RxPacket.h"
#include "TxAllocation.h"
#include "detail/StaticBlockPool.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace {

using Heap = CobsHeapAllocator<1024, 1024>;
using Fixed = CobsFixedAllocator<1024, 8, 1024, 2>;
using Packet = RxPacket<Heap>;
using Ref = PacketRef<Heap>;
using Message = CobsMsg<Heap>;
using Decoder = CobsDecoder;
using Receiver = CobsRx<Heap>;
using Endpoint = Cobs<Heap>;
using FixedEndpoint = Cobs<Fixed>;
using Sender = typename Endpoint::Sender;
using BusyQuery = typename Endpoint::TxBusy;
using RxStats = typename Receiver::Stats;
using TxStats = typename Endpoint::TxStats;
using PoolStats = cobs_detail::PoolStats;

static_assert(sizeof(Ref) == sizeof(void*),
	"a PacketRef is exactly one typed pointer");
static_assert(sizeof(TxAllocation) == sizeof(std::byte*) + sizeof(std::size_t),
	"the transitional TX descriptor is pointer plus capacity");
static_assert(sizeof(Heap) == 1,
	"the stateless heap policy must remain eligible for no_unique_address");

#define COBS_EXPECT_SIZE(Type, Expected) \
	static_assert(sizeof(Type) == (Expected), #Type " layout changed; review ownership and padding")

#if INTPTR_MAX == INT64_MAX
COBS_EXPECT_SIZE(TxAllocation, 16);
COBS_EXPECT_SIZE(Packet, 24);
COBS_EXPECT_SIZE(Ref, 8);
COBS_EXPECT_SIZE(Message, 48);
COBS_EXPECT_SIZE(Decoder, 48);
COBS_EXPECT_SIZE(Receiver, 136);
COBS_EXPECT_SIZE(Endpoint, 304);
COBS_EXPECT_SIZE(Heap, 1);
COBS_EXPECT_SIZE(Fixed, 10496);
COBS_EXPECT_SIZE(FixedEndpoint, 10800);
COBS_EXPECT_SIZE(Sender, 64);
COBS_EXPECT_SIZE(BusyQuery, 64);
COBS_EXPECT_SIZE(RxStats, 28);
COBS_EXPECT_SIZE(TxStats, 12);
COBS_EXPECT_SIZE(PoolStats, 16);
#elif defined(__arm__) && INTPTR_MAX == INT32_MAX
COBS_EXPECT_SIZE(TxAllocation, 8);
COBS_EXPECT_SIZE(Packet, 16);
COBS_EXPECT_SIZE(Ref, 4);
COBS_EXPECT_SIZE(Message, 24);
COBS_EXPECT_SIZE(Decoder, 24);
COBS_EXPECT_SIZE(Receiver, 80);
COBS_EXPECT_SIZE(Endpoint, 168);
COBS_EXPECT_SIZE(Heap, 1);
COBS_EXPECT_SIZE(Fixed, 10424);
COBS_EXPECT_SIZE(FixedEndpoint, 10592);
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
[[maybe_unused]] std::byte cobs_layout_tx_allocation[sizeof(TxAllocation)]{};
[[maybe_unused]] std::byte cobs_layout_rx_packet[sizeof(Packet)]{};
[[maybe_unused]] std::byte cobs_layout_packet_ref[sizeof(Ref)]{};
[[maybe_unused]] std::byte cobs_layout_message[sizeof(Message)]{};
[[maybe_unused]] std::byte cobs_layout_decoder[sizeof(Decoder)]{};
[[maybe_unused]] std::byte cobs_layout_receiver[sizeof(Receiver)]{};
[[maybe_unused]] std::byte cobs_layout_endpoint[sizeof(Endpoint)]{};
[[maybe_unused]] std::byte cobs_layout_heap[sizeof(Heap)]{};
[[maybe_unused]] std::byte cobs_layout_fixed[sizeof(Fixed)]{};
[[maybe_unused]] std::byte cobs_layout_fixed_endpoint[sizeof(FixedEndpoint)]{};
[[maybe_unused]] std::byte cobs_layout_sender[sizeof(Sender)]{};
[[maybe_unused]] std::byte cobs_layout_busy_query[sizeof(BusyQuery)]{};
[[maybe_unused]] std::byte cobs_layout_rx_stats[sizeof(RxStats)]{};
[[maybe_unused]] std::byte cobs_layout_tx_stats[sizeof(TxStats)]{};
[[maybe_unused]] std::byte cobs_layout_pool_stats[sizeof(PoolStats)]{};
}

int main()
{
	std::printf("\n[Layout]\n");
	std::printf("pointer=%zu size_t=%zu TxAllocation=%zu RxPacket=%zu PacketRef=%zu\n",
		sizeof(void*), sizeof(std::size_t), sizeof(TxAllocation), sizeof(Packet), sizeof(Ref));
	std::printf("CobsMsg=%zu CobsDecoder=%zu CobsRx=%zu CobsHeap=%zu\n",
		sizeof(Message), sizeof(Decoder), sizeof(Receiver), sizeof(Endpoint));
	std::printf("Heap=%zu Fixed=%zu CobsFixed=%zu sender=%zu busy=%zu\n",
		sizeof(Heap), sizeof(Fixed), sizeof(FixedEndpoint), sizeof(Sender), sizeof(BusyQuery));
	std::printf("RxStats=%zu TxStats=%zu PoolStats=%zu\n",
		sizeof(RxStats), sizeof(TxStats), sizeof(PoolStats));
	return 0;
}
