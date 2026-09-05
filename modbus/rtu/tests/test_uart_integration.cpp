/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "fake_hal.h"
#include "modbus/rtu/Rtu.h"
#include "Test.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

using namespace modbus_test;

namespace {

struct Fixture final {
	using Serial = Uart<256, 4>;
	using Link = modbus::rtu::Endpoint<wire::Pool<4, 2>>;

	USART_TypeDef usart{};
	DMA_Channel_TypeDef channel_rx{};
	DMA_Channel_TypeDef channel_tx{};
	DMA_HandleTypeDef dma_rx{};
	DMA_HandleTypeDef dma_tx{};
	UART_HandleTypeDef huart{};
	Serial uart{};
	Link link{};

	void configure() noexcept
	{
		huart.Instance = &usart;
		huart.Init.BaudRate = 115200u;
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

	bool start() noexcept
	{
		configure();
		uart.setRxHandler(Serial::RxHandler{
			tiny::bind<&Fixture::on_rx>(*this)});
		uart.setRxGapHandler(Serial::GapHandler{
			tiny::bind<&Fixture::on_gap>(*this)});
		if (!uart.init(&huart)) {
			return false;
		}
		return link.bind(
			Link::Sender{tiny::bind<&Serial::send>(uart)},
			Link::BusyQuery{tiny::bind<&Serial::tx_busy>(uart)});
	}

	void loop() noexcept
	{
		uart.proceed(fake::model().tick);
		link.poll();
	}

	void on_rx(const std::span<const uint8_t> bytes) noexcept
	{
		fake::note_consumer_sees(bytes.data());
		link.receive_adu(bytes);
		fake::note_consumer_done(bytes.data());
	}

	void on_gap() noexcept { link.notify_gap(); }
};

} // namespace

int main()
{
	group("ShortIdleFrame");
	fake::reset();
	Fixture fixture;
	check(fixture.start(), "Uart<256,4> and RTU Endpoint bind without UART changes");
	const auto short_adu = make_adu(1u, 3u,
		std::vector<uint8_t>{0x00u, 0x10u, 0x00u, 0x02u});
	fake::rx_bytes(short_adu.data(), short_adu.size());
	fake::rx_idle();
	fixture.loop();
	auto short_packet = fixture.link.pop_packet();
	check(short_packet && equal(short_packet.adu(), short_adu),
	      "one IDLE-terminated UART burst becomes exactly one RTU candidate");
	short_packet.reset();

	group("Exact256TcFrame");
	std::vector<uint8_t> maximum_data(modbus::max_data_size);
	for (std::size_t i = 0u; i < maximum_data.size(); ++i) {
		maximum_data[i] = static_cast<uint8_t>(i);
	}
	const auto maximum_adu = make_adu(0xF7u, 0x64u, maximum_data);
	check(maximum_adu.size() == 256u, "test input is one maximum RTU ADU");
	fake::rx_bytes(maximum_adu.data(), maximum_adu.size());
	fake::rx_tc();
	// HAL has already re-armed. A later empty IDLE callback must not manufacture
	// a second application candidate.
	fake::rx_idle();
	fixture.loop();
	auto maximum_packet = fixture.link.pop_packet();
	check(maximum_packet && equal(maximum_packet.adu(), maximum_adu),
	      "DMA TC delivers the complete maximum ADU");
	check(!fixture.link.pop_packet() && fixture.link.stats().rx.candidates == 2u,
	      "post-TC empty IDLE produces no bogus Modbus candidate");
	maximum_packet.reset();

	group("CrcDropAndRecovery");
	auto damaged = short_adu;
	damaged[2] ^= 0x01u;
	fake::rx_bytes(damaged.data(), damaged.size());
	fake::rx_idle();
	fixture.loop();
	check(!fixture.link.pop_packet() && fixture.link.stats().rx.crc_errors == 1u,
	      "CRC-invalid physical burst is dropped without scanning within it");
	fake::rx_bytes(short_adu.data(), short_adu.size());
	fake::rx_idle();
	fixture.loop();
	auto recovered_crc = fixture.link.pop_packet();
	check(recovered_crc && equal(recovered_crc.adu(), short_adu),
	      "the next independent physical burst recovers immediately");
	recovered_crc.reset();

	group("OrderedPhysicalGap");
	fake::rx_bytes(short_adu.data(), 3u);
	fake::rx_error(HAL_UART_ERROR_ORE);
	fixture.loop();
	check(!fixture.link.pop_packet() && fixture.link.stats().rx.stream_gaps == 1u,
	      "UART loss produces an ordered gap and no partial Modbus Packet");
	fake::rx_bytes(short_adu.data(), short_adu.size());
	fake::rx_idle();
	fixture.loop();
	auto recovered_gap = fixture.link.pop_packet();
	check(recovered_gap && equal(recovered_gap.adu(), short_adu),
	      "a complete burst after UART recovery is accepted");
	recovered_gap.reset();

	group("BorrowedDmaTx");
	auto tx = fixture.link.make_message(7u, 6u, 2u);
	check(tx.append_be(uint16_t{0x1234u}), "RTU Message builds before UART TX");
	check(fixture.link.send(tx) == modbus::SendResult::Sent &&
	      fixture.uart.tx_busy() && fixture.link.tx_active(),
	      "Endpoint transfers one contiguous ADU borrow into UART DMA");
	check(fake::model().tx_len == 6u &&
	      ::crc::verify<::crc::Crc16Bitwise>(
			std::span<const uint8_t>{fake::model().tx_src, fake::model().tx_len}),
	      "UART DMA sees address/function/data/CRC in the same owned block");
	fake::tx_done();
	fixture.loop();
	check(!fixture.uart.tx_busy() && !fixture.link.tx_active() &&
	      fixture.link.storage().tx_available() == 2u,
	      "poll releases the RTU block after UART stops borrowing it");
	check(fake::model().violations.empty(),
	      "fake HAL observed no DMA/consumer ownership violation");

	return finish();
}
