/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * The real UART driver (uart/Uart.h) against the host fake HAL, integrated
 * with the RTU endpoint through modbus::rtu::UartAdapter — the same object an
 * application uses on the STM32. Two endpoints: the default burst-candidate
 * one, and one with a framing policy, whose stale-frame rule is exercised
 * with the fake HAL's clock at 9600 baud, where one 256-byte DMA chunk takes
 * about 300 ms to arrive.
 */

#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "fake_hal.h"
#include "modbus/rtu/Rtu.h"
#include "modbus/rtu/UartAdapter.h"
#include "Test.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace modbus_test;
namespace framing = modbus::rtu::framing;

namespace {

using Serial = Uart<256, 4>;

static_assert(modbus::rtu::UartTraits<Serial>::chunk_size == 256u);
static_assert(modbus::rtu::UartTraits<Serial>::chunk_count == 4u);

// A private function with a library-owned length prefix: the only way to
// build frames longer than one DMA chunk inside a 1024-byte private ADU.
struct WideFramer : framing::Standard<framing::Direction::Request> {
	using Base = framing::Standard<framing::Direction::Request>;

	[[nodiscard]] static constexpr framing::Layout layout(
			const framing::Direction direction,
			const uint8_t function) noexcept
	{
		return function == 0x41u ? framing::Layout::length_prefixed(2u)
		                         : Base::layout(direction, function);
	}
};

template<class LinkT>
struct Fixture final {
	using Link = LinkT;
	using Adapter = modbus::rtu::UartAdapter<Serial, Link>;

	USART_TypeDef usart{};
	DMA_Channel_TypeDef channel_rx{};
	DMA_Channel_TypeDef channel_tx{};
	DMA_HandleTypeDef dma_rx{};
	DMA_HandleTypeDef dma_tx{};
	UART_HandleTypeDef huart{};
	Serial uart{};
	Link link{};
	Adapter adapter;

	explicit Fixture(const uint32_t baud) noexcept : adapter(uart, link, baud)
	{
		huart.Init.BaudRate = baud;
	}

	void configure() noexcept
	{
		huart.Instance = &usart;
		huart.Init.WordLength = UART_WORDLENGTH_8B;
		huart.Init.StopBits = UART_STOPBITS_1;
		huart.Init.Parity = UART_PARITY_NONE;
		huart.Init.Mode = UART_MODE_TX_RX;
		huart.Init.HwFlowCtl = UART_HWCONTROL_NONE;
		huart.gState = HAL_UART_STATE_READY;
		huart.RxState = HAL_UART_STATE_READY;
		huart.ErrorCode = HAL_UART_ERROR_NONE;

		dma_rx.Instance = &channel_rx;
		dma_rx.State = HAL_DMA_STATE_READY;
		dma_rx.Parent = &huart;
		dma_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
		dma_rx.Init.PeriphInc = DMA_PINC_DISABLE;
		dma_rx.Init.MemInc = DMA_MINC_ENABLE;
		dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
		dma_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
		dma_rx.Init.Mode = DMA_NORMAL;

		dma_tx = dma_rx;
		dma_tx.Instance = &channel_tx;
		dma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
		huart.hdmarx = &dma_rx;
		huart.hdmatx = &dma_tx;
		fake::model().huart = &huart;
	}

	// The application's whole setup: driver init and one adapter bind.
	bool start() noexcept
	{
		configure();
		return uart.init(&huart) && adapter.bind();
	}

	// Like start(), but the RX handler is then re-pointed at a wrapper that
	// tells the fake HAL which buffer the consumer touches, so its DMA
	// ownership model can assert that no DMA-owned memory reaches the endpoint.
	// The plain start() proves the adapter's own RX routing; this proves the
	// ownership discipline of the framed copy path under it.
	bool start_observed() noexcept
	{
		if (!start()) {
			return false;
		}
		uart.setRxHandler(typename Serial::RxHandler{
			tiny::bind<&Fixture::observed_rx>(*this)});
		return true;
	}

	void observed_rx(const std::span<const uint8_t> bytes) noexcept
	{
		fake::note_consumer_sees(bytes.data());
		adapter.on_rx(bytes);
		fake::note_consumer_done(bytes.data());
	}

	// The application's whole loop step.
	void loop() noexcept { adapter.proceed(fake::model().tick); }

	std::vector<uint8_t> pop_adu() noexcept
	{
		auto packet = link.pop_packet();
		return packet ? std::vector<uint8_t>(packet.adu().begin(), packet.adu().end())
		              : std::vector<uint8_t>{};
	}
};

using BurstLink = modbus::rtu::Endpoint<wire::Pool<4, 2>>;
using FramedLink = modbus::rtu::Endpoint<wire::Pool<4, 2>,
	modbus::rtu::Format<::crc::Crc16Bitwise, 1024>, WideFramer>;

std::vector<uint8_t> wide_frame(const std::size_t body_size, const uint8_t seed)
{
	std::vector<uint8_t> data(2u + body_size);
	data[0] = static_cast<uint8_t>(body_size >> 8u);
	data[1] = static_cast<uint8_t>(body_size);
	for (std::size_t i = 0u; i < body_size; ++i) {
		data[2u + i] = static_cast<uint8_t>(i * 7u + seed);
	}
	return make_adu(0x11u, 0x41u, data);
}

void feed(const std::span<const uint8_t> bytes, const bool full_chunk)
{
	fake::rx_bytes(bytes.data(), bytes.size());
	if (full_chunk) {
		fake::rx_tc();
	} else {
		fake::rx_idle();
	}
}

} // namespace

int main()
{
	group("BurstEndpointThroughAdapter");
	fake::reset();
	Fixture<BurstLink> burst{115200u};
	check(burst.start(), "Uart<256,4>, default RTU endpoint and adapter bind in one call");
	const auto short_adu = make_adu(1u, 3u, std::vector<uint8_t>{0x00u, 0x10u, 0x00u, 0x02u});
	feed(short_adu, false);
	burst.loop();
	check(equal(burst.pop_adu(), short_adu),
	      "one IDLE-terminated UART burst becomes exactly one RTU candidate");

	std::vector<uint8_t> maximum_data(modbus::max_data_size);
	for (std::size_t i = 0u; i < maximum_data.size(); ++i) {
		maximum_data[i] = static_cast<uint8_t>(i);
	}
	const auto maximum_adu = make_adu(0xF7u, 0x64u, maximum_data);
	check(maximum_adu.size() == 256u, "test input is one maximum RTU ADU");
	feed(maximum_adu, true);
	// HAL has already re-armed. A later empty IDLE callback must not manufacture
	// a second application candidate.
	fake::rx_idle();
	burst.loop();
	check(equal(burst.pop_adu(), maximum_adu), "DMA TC delivers the complete maximum ADU");
	check(burst.pop_adu().empty() && burst.link.stats().rx.candidates == 2u,
	      "post-TC empty IDLE produces no bogus Modbus candidate");

	auto damaged = short_adu;
	damaged[2] ^= 0x01u;
	feed(damaged, false);
	burst.loop();
	check(burst.pop_adu().empty() && burst.link.stats().rx.crc_errors == 1u,
	      "CRC-invalid physical burst is dropped without scanning within it");
	feed(short_adu, false);
	burst.loop();
	check(equal(burst.pop_adu(), short_adu), "the next independent physical burst recovers immediately");

	fake::rx_bytes(short_adu.data(), 3u);
	fake::rx_error(HAL_UART_ERROR_ORE);
	burst.loop();
	check(burst.pop_adu().empty() && burst.link.stats().rx.stream_gaps == 1u,
	      "UART loss reaches notify_gap() through the adapter and no partial Packet appears");
	feed(short_adu, false);
	burst.loop();
	check(equal(burst.pop_adu(), short_adu), "a complete burst after UART recovery is accepted");

	auto tx = burst.link.make_message(7u, 6u, 2u);
	check(tx.append_be(uint16_t{0x1234u}), "RTU Message builds before UART TX");
	check(burst.link.send(tx) == modbus::SendResult::Sent &&
	      burst.uart.tx_busy() && burst.link.tx_active(),
	      "the adapter's transport binding hands one contiguous ADU to UART DMA");
	check(fake::model().tx_len == 6u &&
	      ::crc::verify<::crc::Crc16Bitwise>(
			std::span<const uint8_t>{fake::model().tx_src, fake::model().tx_len}),
	      "UART DMA sees address/function/data/CRC in the same owned block");
	fake::tx_done();
	burst.loop();
	check(!burst.uart.tx_busy() && !burst.link.tx_active() &&
	      burst.link.storage().tx_available() == 2u,
	      "adapter.proceed() releases the RTU block after UART stops borrowing it");
	check(fake::model().violations.empty(), "fake HAL observed no DMA/consumer ownership violation");

	group("FramedEndpointAt9600");
	fake::reset();
	Fixture<FramedLink> framed{9600u};
	check(framed.start_observed(), "Uart<256,4>, framed 1024-byte endpoint and adapter bind");
	check(Fixture<FramedLink>::Adapter::chunk_time_ms(9600u) == 294u &&
	      framed.adapter.full_chunk_ms() == 294u,
	      "one 256-byte chunk at 9600 baud is 294 ms of 11-bit characters, rounded up");
	check(Fixture<FramedLink>::Adapter::chunk_time_ms(1000000u) == 3u &&
	      Fixture<FramedLink>::Adapter::chunk_time_ms(0u) == 0u,
	      "3 ms at 1M; 0 for an invalid baud");
	{
		// A 700-byte ADU: two full chunks and a partial one, each about 290 ms
		// apart — the transfer time of a chunk at this baud. The frame must
		// stay in flight across the silences that are really the line still
		// busy filling the next chunk.
		const auto adu = wide_frame(694u, 1u);
		check(adu.size() == 700u, "700-byte private ADU built");
		const std::span<const uint8_t> bytes{adu};
		feed(bytes.first(256u), true);
		framed.loop();
		check(framed.link.assembling() && framed.adapter.deadline_armed() &&
		      framed.adapter.deadline_in_ms(fake::model().tick) == 294u + 5u,
		      "after a full chunk the deadline is one chunk time plus the guard");
		fake::advance_tick(290u);
		framed.loop();
		check(framed.link.assembling() && framed.link.framing_stats().stale_frames == 0u,
		      "290 ms of software silence after a full chunk is the line at work, not a dead frame");
		feed(bytes.subspan(256u, 256u), true);
		framed.loop();
		fake::advance_tick(290u);
		framed.loop();
		feed(bytes.subspan(512u), false);
		framed.loop();
		check(equal(framed.pop_adu(), adu), "the 700-byte frame arrives whole across three chunks");
		check(!framed.adapter.deadline_armed() &&
		      framed.adapter.deadline_in_ms(fake::model().tick) == Fixture<FramedLink>::Adapter::no_deadline,
		      "a completed frame disarms the deadline");
		check(framed.link.framing_stats().stale_frames == 0u, "nothing was expired");
	}
	{
		// The sender dies exactly on a chunk boundary: no IDLE ever fires, only
		// the full-chunk deadline can catch it.
		const auto adu = wide_frame(694u, 2u);
		feed(std::span<const uint8_t>{adu}.first(256u), true);
		framed.loop();
		fake::advance_tick(298u);
		framed.loop();
		check(framed.link.assembling(), "298 ms: still within one chunk time plus the guard");
		fake::advance_tick(1u);
		framed.loop();
		check(!framed.link.assembling() && framed.link.framing_stats().stale_frames == 1u &&
		      framed.link.storage().rx_available() == 4u,
		      "299 ms: the frame is expired and its block returned");
	}
	{
		// An orphan half ended by IDLE: the line is silent, five milliseconds
		// decide.
		const auto adu = wide_frame(300u, 3u);
		feed(std::span<const uint8_t>{adu}.first(100u), false);
		framed.loop();
		check(framed.adapter.deadline_in_ms(fake::model().tick) == 5u,
		      "after a partial chunk the deadline is the idle limit");
		fake::advance_tick(4u);
		framed.loop();
		check(framed.link.assembling(), "4 ms: alive");
		fake::advance_tick(1u);
		framed.loop();
		check(!framed.link.assembling() && framed.link.framing_stats().stale_frames == 2u,
		      "5 ms: expired");
		// The next frame is unaffected by the orphan.
		const auto next = wide_frame(50u, 4u);
		feed(next, false);
		framed.loop();
		check(equal(framed.pop_adu(), next), "the frame after an orphan is delivered");
	}
	{
		// A bridge-like split: a partial chunk resumed 1 ms later.
		const auto adu = wide_frame(300u, 5u);
		const std::span<const uint8_t> bytes{adu};
		feed(bytes.first(150u), false);
		framed.loop();
		fake::advance_tick(1u);
		feed(bytes.subspan(150u), false);
		framed.loop();
		check(equal(framed.pop_adu(), adu) && framed.link.framing_stats().stale_frames == 2u,
		      "a 1 ms split is reassembled, not expired");
	}
	{
		// A UART loss mid-frame: the ordered gap reaches notify_gap() and
		// disarms the deadline; the next complete frame is accepted.
		const auto adu = wide_frame(300u, 6u);
		fake::rx_bytes(adu.data(), 100u);
		fake::rx_error(HAL_UART_ERROR_ORE);
		framed.loop();
		check(!framed.adapter.deadline_armed() && !framed.link.assembling() &&
		      framed.link.stats().rx.stream_gaps == 1u && framed.link.storage().rx_available() == 4u,
		      "a gap during a frame releases it through the adapter");
		const auto next = wide_frame(60u, 7u);
		feed(next, false);
		framed.loop();
		check(equal(framed.pop_adu(), next), "the frame after the gap is delivered");
	}
	{
		// Two frames glued in one full chunk plus a tail: the first is
		// delivered from the chunk, the second completes from the next one.
		const auto first = wide_frame(100u, 8u);   // 106 bytes
		const auto second = wide_frame(300u, 9u);  // 306 bytes
		std::vector<uint8_t> glued(first);
		glued.insert(glued.end(), second.begin(), second.end());
		const std::span<const uint8_t> bytes{glued};
		feed(bytes.first(256u), true);
		framed.loop();
		check(equal(framed.pop_adu(), first) && framed.link.assembling(),
		      "the first glued frame is delivered while the second waits for its chunk");
		fake::advance_tick(250u);
		framed.loop();
		feed(bytes.subspan(256u), false);
		framed.loop();
		check(equal(framed.pop_adu(), second), "the second glued frame completes from the partial chunk");
		check(framed.link.framing_stats().stale_frames == 2u && framed.link.storage().rx_available() == 4u,
		      "no expiry, no leak");
	}
	{
		// Transmit through the adapter's binding on the framed endpoint: the
		// library fills the length prefix before the CRC.
		auto reply = framed.link.make_message(0x11u, 0x41u);
		const std::vector<uint8_t> body{1u, 2u, 3u};
		check(reply.size() == 2u && reply.append_bytes(body) &&
		      framed.link.send(reply) == modbus::SendResult::Sent,
		      "a prefixed private frame is sent");
		check(fake::model().tx_len == 2u + 2u + 3u + 2u && fake::model().tx_src[2] == 0u &&
		      fake::model().tx_src[3] == 3u,
		      "the prefix on the wire holds the body length");
		fake::tx_done();
		framed.loop();
		check(!framed.link.tx_active(), "adapter.proceed() reclaims the transmitted block");
	}
	check(fake::model().violations.empty(), "fake HAL observed no ownership violation on the framed link");

	group("AdapterRefusesAnInvalidBaud");
	{
		fake::reset();
		Fixture<FramedLink> broken{0u};
		broken.configure();
		check(!broken.adapter.bind(), "bind() refuses a baud of 0: no deadline can be computed");
	}

	return finish();
}
