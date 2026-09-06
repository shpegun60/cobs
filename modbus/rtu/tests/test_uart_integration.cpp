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
 * 320 ms of 12-bit characters to arrive. The adapter's lifecycle contract is
 * covered too: construction before the handle carries a rate, transactional
 * bind()/unbind(), a changed line rate, a continuation queued at the
 * deadline, the tick wrapping around, and the case the 5 ms rule alone gets
 * wrong: a bridge that resumes a split frame into the next DMA chunk before
 * any IDLE or TC event has fired, which the driver's rx_progress() exposes.
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

// Counts chunks that reach a handler the application installed itself: the
// proof that a failed bind() left the driver's handlers alone.
struct Sentinel final {
	unsigned chunks = 0;
	void on_rx(std::span<const uint8_t>) noexcept { ++chunks; }
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
	UART_HandleTypeDef huart{};   // zero-initialized, like a CubeMX global before MX_USARTx_UART_Init()
	Serial uart{};
	Link link{};
	Adapter adapter{uart, link};  // constructed while huart.Init.BaudRate is still 0

	void configure(const uint32_t baud) noexcept
	{
		huart.Instance = &usart;
		huart.Init.BaudRate = baud;
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

	// The application's whole setup: configure, driver init, one adapter bind.
	bool start(const uint32_t baud) noexcept
	{
		configure(baud);
		return uart.init(&huart) && adapter.bind();
	}

	// Like start(), but the RX handler is then re-pointed at a wrapper that
	// tells the fake HAL which buffer the consumer touches, so its DMA
	// ownership model can assert that no DMA-owned memory reaches the endpoint.
	// The plain start() proves the adapter's own RX routing; this proves the
	// ownership discipline of the framed copy path under it.
	bool start_observed(const uint32_t baud) noexcept
	{
		if (!start(baud)) {
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
using FramedAdapter = Fixture<FramedLink>::Adapter;

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
	Fixture<BurstLink> burst;
	check(burst.start(115200u), "Uart<256,4>, default RTU endpoint and adapter bind in one call");
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
	check(!burst.adapter.unbind(), "unbind() is refused while the driver still borrows the frame");
	fake::tx_done();
	burst.loop();
	check(!burst.uart.tx_busy() && !burst.link.tx_active() &&
	      burst.link.storage().tx_available() == 2u,
	      "adapter.proceed() releases the RTU block after UART stops borrowing it");
	check(fake::model().violations.empty(), "fake HAL observed no DMA/consumer ownership violation");

	group("BindLifecycle");
	{
		// Construction happened with a zero-initialized handle (the CubeMX
		// static-init situation); binding before the driver is initialized must
		// fail without side effects, and succeed afterwards with the rate read
		// from the handle.
		fake::reset();
		Fixture<FramedLink> f;
		Sentinel sentinel;
		f.uart.setRxHandler(Serial::RxHandler{tiny::bind<&Sentinel::on_rx>(sentinel)});
		check(!f.adapter.bind() && !f.adapter.bound(), "bind() before Uart::init() is refused");
		f.configure(9600u);
		check(f.uart.init(&f.huart), "driver initialized at 9600");
		feed(short_adu, false);
		f.loop();
		check(sentinel.chunks == 1u && !f.link.has_packet(),
		      "the refused bind() left the application's own RX handler in place");
		check(f.adapter.bind() && f.adapter.bound() && f.adapter.baud() == 9600u &&
		      f.adapter.full_chunk_ms() == 320u,
		      "bind() after init reads 9600 baud from the handle: 320 ms per 256-byte chunk of 12-bit characters");
		check(FramedAdapter::chunk_time_ms(1000000u) == 4u && FramedAdapter::chunk_time_ms(0u) == 0u,
		      "4 ms at 1M; 0 for an invalid rate");
		const auto frame = wide_frame(20u, 1u);
		feed(frame, false);
		f.loop();
		check(equal(f.pop_adu(), frame) && sentinel.chunks == 1u, "after bind() the adapter receives, the sentinel no longer does");

		// A failed bind() must not touch the driver's handlers: with a
		// transmission in flight the endpoint refuses the transport binding.
		auto tx2 = f.link.make_message(0x11u, 0x41u);
		check(tx2.append_bytes(std::vector<uint8_t>{1u, 2u}) && f.link.send(tx2) == modbus::SendResult::Sent,
		      "a transmission is in flight");
		FramedAdapter second{f.uart, f.link};
		check(!second.bind() && !second.bound(), "a second adapter's bind() is refused while TX is active");
		feed(frame, false);
		f.loop();
		check(equal(f.pop_adu(), frame), "and the first adapter still receives: the refused bind() changed nothing");
		check(!f.adapter.unbind(), "unbind() is refused too while TX is active");
		fake::tx_done();
		f.loop();
		feed(std::span<const uint8_t>{frame}.first(10u), false);   // a frame in flight
		f.loop();
		check(f.link.assembling(), "a partial frame is in flight");
		check(f.adapter.unbind() && !f.adapter.bound(), "unbind() succeeds once the frame is released");
		check(!f.link.assembling() && f.link.framing_stats().stale_frames == 0u &&
		      f.link.storage().rx_available() == 4u,
		      "unbind() discards the frame in flight as a discontinuity: block returned, nothing counted");
		feed(frame, false);
		f.loop();
		check(!f.link.has_packet(), "after unbind() nothing reaches the endpoint");
		check(f.adapter.bind(), "and bind() works again");
		feed(frame, false);
		f.loop();
		check(equal(f.pop_adu(), frame), "the first frame after the rebind is delivered whole, glued to nothing");
		check(fake::model().violations.empty(), "no ownership violation across the lifecycle");
	}

	group("FramedEndpointAt9600");
	fake::reset();
	Fixture<FramedLink> framed;
	check(framed.start_observed(9600u), "Uart<256,4>, framed 1024-byte endpoint and adapter bind");
	{
		// A 700-byte ADU: two full chunks and a partial one, each about 310 ms
		// apart — the transfer time of a chunk at this baud. The frame must
		// stay in flight across the silences that are really the line still
		// busy filling the next chunk.
		const auto adu = wide_frame(694u, 1u);
		check(adu.size() == 700u, "700-byte private ADU built");
		const std::span<const uint8_t> bytes{adu};
		feed(bytes.first(256u), true);
		framed.loop();
		check(framed.link.assembling() && framed.adapter.deadline_armed() &&
		      framed.adapter.deadline_in_ms(fake::model().tick) == 320u + 5u,
		      "after a full chunk the deadline is one chunk time plus the guard");
		fake::advance_tick(310u);
		framed.loop();
		check(framed.link.assembling() && framed.link.framing_stats().stale_frames == 0u,
		      "310 ms of software silence after a full chunk is the line at work, not a dead frame");
		feed(bytes.subspan(256u, 256u), true);
		framed.loop();
		fake::advance_tick(310u);
		framed.loop();
		feed(bytes.subspan(512u), false);
		framed.loop();
		check(equal(framed.pop_adu(), adu), "the 700-byte frame arrives whole across three chunks");
		check(!framed.adapter.deadline_armed() &&
		      framed.adapter.deadline_in_ms(fake::model().tick) == FramedAdapter::no_deadline,
		      "a completed frame disarms the deadline");
		check(framed.link.framing_stats().stale_frames == 0u, "nothing was expired");
	}
	{
		// The sender dies exactly on a chunk boundary: no IDLE ever fires, only
		// the full-chunk deadline can catch it.
		const auto adu = wide_frame(694u, 2u);
		feed(std::span<const uint8_t>{adu}.first(256u), true);
		framed.loop();
		fake::advance_tick(324u);
		framed.loop();
		check(framed.link.assembling(), "324 ms: still within one chunk time plus the guard");
		fake::advance_tick(1u);
		framed.loop();
		check(!framed.link.assembling() && framed.link.framing_stats().stale_frames == 1u &&
		      framed.link.storage().rx_available() == 4u,
		      "325 ms: the frame is expired and its block returned");
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
		const auto next = wide_frame(50u, 4u);
		feed(next, false);
		framed.loop();
		check(equal(framed.pop_adu(), next), "the frame after an orphan is delivered");
	}
	{
		// A continuation that is already queued in the driver when the deadline
		// falls due must be delivered, not outrun: proceed() drains the driver
		// before it judges the frame.
		const auto adu = wide_frame(300u, 5u);
		const std::span<const uint8_t> bytes{adu};
		feed(bytes.first(150u), false);
		framed.loop();
		fake::advance_tick(5u);
		feed(bytes.subspan(150u), false);   // arrives before the loop runs at t+5
		framed.loop();
		check(equal(framed.pop_adu(), adu) && framed.link.framing_stats().stale_frames == 2u,
		      "a continuation queued at the deadline completes the frame instead of expiring it");
		// The same with the instrumented composition of the steps.
		feed(bytes.first(150u), false);
		framed.adapter.prepare(fake::model().tick);
		framed.uart.proceed(fake::model().tick);
		framed.adapter.finish(fake::model().tick);
		framed.link.poll(fake::model().tick);
		fake::advance_tick(5u);
		feed(bytes.subspan(150u), false);
		framed.adapter.prepare(fake::model().tick);
		framed.uart.proceed(fake::model().tick);
		framed.adapter.finish(fake::model().tick);
		framed.link.poll(fake::model().tick);
		check(equal(framed.pop_adu(), adu) && framed.link.framing_stats().stale_frames == 2u,
		      "prepare -> uart.proceed -> finish -> poll keeps that order");
	}
	{
		// A bridge-like split: a partial chunk resumed 1 ms later.
		const auto adu = wide_frame(300u, 6u);
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
		const auto adu = wide_frame(300u, 7u);
		fake::rx_bytes(adu.data(), 100u);
		fake::rx_error(HAL_UART_ERROR_ORE);
		framed.loop();
		check(!framed.adapter.deadline_armed() && !framed.link.assembling() &&
		      framed.link.stats().rx.stream_gaps == 1u && framed.link.storage().rx_available() == 4u,
		      "a gap during a frame releases it through the adapter");
		const auto next = wide_frame(60u, 8u);
		feed(next, false);
		framed.loop();
		check(equal(framed.pop_adu(), next), "the frame after the gap is delivered");
	}
	{
		// Two frames glued in one full chunk plus a tail: the first is
		// delivered from the chunk, the second completes from the next one.
		const auto first = wide_frame(100u, 9u);   // 106 bytes
		const auto second = wide_frame(300u, 10u); // 306 bytes
		std::vector<uint8_t> glued(first);
		glued.insert(glued.end(), second.begin(), second.end());
		const std::span<const uint8_t> bytes{glued};
		feed(bytes.first(256u), true);
		framed.loop();
		check(equal(framed.pop_adu(), first) && framed.link.assembling(),
		      "the first glued frame is delivered while the second waits for its chunk");
		fake::advance_tick(300u);
		framed.loop();
		feed(bytes.subspan(256u), false);
		framed.loop();
		check(equal(framed.pop_adu(), second), "the second glued frame completes from the partial chunk");
		check(framed.link.framing_stats().stale_frames == 2u && framed.link.storage().rx_available() == 4u,
		      "no expiry, no leak");
	}
	{
		// The millisecond tick wraps: deadlines are differences, never
		// comparisons of absolute values.
		fake::model().tick = 0xFFFFFFFEu;
		const auto adu = wide_frame(300u, 11u);
		feed(std::span<const uint8_t>{adu}.first(100u), false);
		framed.loop();                                   // deadline at 3, past the wrap
		check(framed.adapter.deadline_in_ms(0xFFFFFFFEu) == 5u, "5 ms left at 0xFFFFFFFE");
		fake::model().tick = 0xFFFFFFFFu;
		framed.loop();
		check(framed.link.assembling(), "1 ms before the wrap: alive");
		fake::model().tick = 2u;
		framed.loop();
		check(framed.link.assembling() && framed.adapter.deadline_in_ms(2u) == 1u, "1 ms after the wrap: alive, 1 ms left");
		fake::model().tick = 3u;
		framed.loop();
		check(!framed.link.assembling() && framed.link.framing_stats().stale_frames == 3u,
		      "5 ms across the wrap: expired");
		fake::model().tick = 100000u;
	}
	{
		// The line rate changes at runtime through the driver: the adapter
		// follows the handle on the next proceed(), with no second call.
		const auto adu = wide_frame(694u, 12u);
		check(framed.uart.setBaudRate(1000000u) && framed.huart.Init.BaudRate == 1000000u,
		      "the driver reconfigures to 1M");
		framed.loop();                                   // the rate change is a gap; proceed() follows the new rate
		check(framed.adapter.baud() == 1000000u && framed.adapter.full_chunk_ms() == 4u,
		      "the adapter now times chunks at 1M: 4 ms");
		feed(std::span<const uint8_t>{adu}.first(256u), true);
		framed.loop();
		check(framed.adapter.deadline_in_ms(fake::model().tick) == 4u + 5u,
		      "a full chunk at 1M gets a 9 ms deadline, not the 325 ms of 9600");
		fake::advance_tick(9u);
		framed.loop();
		check(!framed.link.assembling() && framed.link.framing_stats().stale_frames == 4u,
		      "and is expired accordingly when nothing follows");
		check(framed.uart.setBaudRate(9600u), "back to 9600");
		framed.loop();
		check(framed.adapter.full_chunk_ms() == 320u, "followed again");
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

	group("DestructorDetaches");
	{
		fake::reset();
		Fixture<FramedLink> f;
		check(f.start(115200u), "bound");
		{
			FramedAdapter temporary{f.uart, f.link};
			check(temporary.bind() && temporary.bound(),
			      "a second adapter binds over the first while no transmission is active");
			const auto partial = wide_frame(30u, 14u);
			feed(std::span<const uint8_t>{partial}.first(12u), false);
			temporary.proceed(fake::model().tick);
			check(f.link.assembling(), "a frame is in flight through the temporary adapter");
			// `temporary` now owns the driver's handlers and dies here: it must
			// detach them on the way out rather than leave them pointing at a
			// dead object, and drop the frame it can no longer complete.
		}
		check(!f.link.assembling() && f.link.framing_stats().stale_frames == 0u,
		      "the dying adapter discarded the frame in flight, uncounted");
		const auto frame = wide_frame(20u, 13u);
		feed(frame, false);
		f.loop();
		check(!f.link.has_packet(), "after the bound adapter died nothing is routed into its dead this-pointer");
		check(f.adapter.bind(), "the surviving adapter binds again");
		feed(frame, false);
		f.loop();
		check(equal(f.pop_adu(), frame), "and receives");
	}

	group("PartialIdleThenDmaResumes");
	{
		// The scenario the 5 ms rule alone gets wrong. A bridge splits a frame:
		// the first part ends with IDLE at t0, the bridge resumes 1 ms later
		// and the remainder is physically arriving into the next DMA chunk,
		// but no IDLE or TC has fired yet, so the driver has nothing to
		// publish when the deadline falls due at t0 + 5 ms. At 115200 the
		// remainder of a 300-byte frame takes about 14 ms on the wire; at
		// 9600 more than a hundred. The frame must stay alive: the driver's
		// DMA counter shows the line is busy again.
		fake::reset();
		Fixture<FramedLink> f;
		check(f.start(115200u), "framed endpoint through the adapter at 115200");
		check(f.adapter.full_chunk_ms() == 27u, "one 256-byte chunk of 12-bit characters at 115200: 27 ms");
		const auto adu = wide_frame(300u, 21u);
		const std::span<const uint8_t> bytes{adu};
		feed(bytes.first(150u), false);
		f.loop();
		check(f.link.assembling() && f.adapter.deadline_in_ms(fake::model().tick) == 5u,
		      "the first part ends with IDLE: 5 ms deadline");
		fake::advance_tick(1u);
		fake::rx_bytes(bytes.data() + 150u, 100u);   // DMA is receiving again; no event yet
		fake::advance_tick(4u);
		f.loop();
		check(f.link.assembling() && f.link.framing_stats().stale_frames == 0u,
		      "at the deadline DMA has already taken 100 bytes of the remainder: the frame is alive");
		check(f.adapter.deadline_in_ms(fake::model().tick) == 27u + 5u,
		      "the deadline moved to one chunk time plus the guard, as after a full chunk");
		fake::advance_tick(10u);
		f.loop();
		check(f.link.assembling() && f.link.framing_stats().stale_frames == 0u,
		      "10 ms later, still no event: still alive");
		feed(bytes.subspan(250u), false);            // the remainder ends with IDLE
		f.loop();
		check(equal(f.pop_adu(), adu) && f.link.framing_stats().stale_frames == 0u,
		      "the frame arrives whole");

		// The orphan: IDLE, then nothing at all on the line. Progress stays 0,
		// and 5 ms is the verdict exactly as before.
		feed(bytes.first(150u), false);
		f.loop();
		fake::advance_tick(5u);
		f.loop();
		check(!f.link.assembling() && f.link.framing_stats().stale_frames == 1u,
		      "with nothing arriving after IDLE the orphan is expired at 5 ms");

		// The instant of the deadline. The progress snapshot is taken BEFORE the
		// driver is drained and the verdict AFTER, so a continuation the driver
		// publishes in between is delivered, never outrun by its own deadline.
		feed(bytes.first(150u), false);
		f.loop();
		fake::advance_tick(5u);
		f.adapter.prepare(fake::model().tick);       // snapshot: no progress, deadline due
		feed(bytes.subspan(150u), false);            // the ISR publishes the remainder now
		f.uart.proceed(fake::model().tick);
		f.adapter.finish(fake::model().tick);
		f.link.poll(fake::model().tick);
		check(equal(f.pop_adu(), adu) && f.link.framing_stats().stale_frames == 1u,
		      "a continuation published between the snapshot and the drain completes the frame");

		// Bytes that only begin to arrive after the deadline instant are late by
		// definition: the orphan is expired, and the late remainder is a
		// garbage frame start the endpoint resynchronizes from.
		feed(bytes.first(150u), false);
		f.loop();
		fake::advance_tick(5u);
		f.adapter.prepare(fake::model().tick);
		f.uart.proceed(fake::model().tick);
		fake::rx_bytes(bytes.data() + 150u, 100u);   // arrives only now, unpublished
		f.adapter.finish(fake::model().tick);
		f.link.poll(fake::model().tick);
		check(!f.link.assembling() && f.link.framing_stats().stale_frames == 2u,
		      "bytes first arriving after the deadline instant do not rescue the orphan");
		feed(bytes.subspan(250u), false);
		f.loop();
		const auto next = wide_frame(40u, 22u);
		feed(next, false);
		f.loop();
		check(equal(f.pop_adu(), next) && f.link.storage().rx_available() == 4u,
		      "the endpoint resynchronizes on the next frame and leaks nothing");
		check(fake::model().violations.empty(), "no ownership violation");
	}

	return finish();
}
