/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

// Same UART lifecycle through the COBS and RTU adapters, using the REAL UART
// driver with the recording HAL, not a no-op serial mock.
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "fake_hal.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/cobs/UartAdapter.h"
#include "adapters/rtu/UartAdapter.h"
#include "adapters/freertos/FreeRtosWake.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

namespace {
unsigned checks = 0u, failures = 0u;
void check(bool ok, const char* label)
{
	++checks;
	if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}
using Serial = Uart<256u, 4u>;
struct Hardware {
	USART_TypeDef usart{};
	DMA_Channel_TypeDef rx_channel{}, tx_channel{};
	DMA_HandleTypeDef rx{}, tx{};
	UART_HandleTypeDef handle{};
	void configure()
	{
		handle.Instance = &usart;
		handle.Init.BaudRate = 115200u;
		handle.Init.WordLength = UART_WORDLENGTH_8B;
		handle.Init.StopBits = UART_STOPBITS_1;
		handle.Init.Parity = UART_PARITY_NONE;
		handle.Init.Mode = UART_MODE_TX_RX;
		handle.Init.HwFlowCtl = UART_HWCONTROL_NONE;
		handle.gState = HAL_UART_STATE_READY;
		handle.RxState = HAL_UART_STATE_READY;
		rx.Instance = &rx_channel;
		rx.State = HAL_DMA_STATE_READY;
		rx.Parent = &handle;
		rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
		rx.Init.PeriphInc = DMA_PINC_DISABLE;
		rx.Init.MemInc = DMA_MINC_ENABLE;
		rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
		rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
		rx.Init.Mode = DMA_NORMAL;
		tx = rx;
		tx.Instance = &tx_channel;
		tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
		handle.hdmarx = &rx;
		handle.hdmatx = &tx;
		fake::model().huart = &handle;
	}
};
struct Sentinel {
	unsigned chunks = 0u, gaps = 0u;
	void on_rx(std::span<const uint8_t>) noexcept { ++chunks; }
	void on_gap() noexcept { ++gaps; }
};
template<class E> auto make(E& endpoint)
{
	if constexpr (requires { endpoint.make_message(4u); }) { return endpoint.make_message(4u); }
	else { return endpoint.make_message(0x11u, 0x06u, 4u); }
}

template<class E, template<class, class> class A> void exercise(const char* label)
{
	std::printf("UART adapter parity: %s\n", label);
	fake::reset();
	fake_freertos::reset();
	Hardware hardware;
	uart::FreeRtosWake wake; // outlives the driver's reference to its ISR callback
	Serial serial;
	E endpoint;
	Sentinel sentinel;
	std::vector<uint8_t> frame;
	constexpr std::array<uint8_t, 4> data{0x12u, 0x34u, 0x56u, 0x78u};
	using Adapter = A<Serial, E>;
	{
		Adapter adapter{serial, endpoint};
		serial.setRxHandler(Serial::RxHandler{tiny::bind<&Sentinel::on_rx>(sentinel)});
		serial.setRxGapHandler(Serial::GapHandler{tiny::bind<&Sentinel::on_gap>(sentinel)});
		check(!adapter.bind() && !adapter.bound(), "refuse bind before driver initialization");
		hardware.configure();
		check(serial.init(&hardware.handle), "real UART driver initializes");
		const auto baud = hardware.handle.Init.BaudRate;
		hardware.handle.Init.BaudRate = 0u;
		check(!adapter.bind(), "zero-rate handle is refused without handler mutation");
		hardware.handle.Init.BaudRate = baud;
		fake::rx_bytes(data.data(), data.size());
		fake::rx_idle();
		adapter.proceed(0u);
		check(sentinel.chunks == 1u && !endpoint.has_packet(), "failed bind preserves prior RX handler");
		fake::rx_error(HAL_UART_ERROR_ORE);
		adapter.proceed(0u);
		check(sentinel.gaps == 1u, "failed bind preserves prior gap handler");
		check(adapter.bind() && adapter.bound(), "bind succeeds after init");
		check(adapter.bind(), "idle rebinding to same objects is supported");
		auto* const task = reinterpret_cast<tskTaskControlBlock*>(0x20001000u);
		check(wake.attach(serial, task), "same FreeRtosWake attaches for either protocol");
		check(!adapter.deadline_armed() && adapter.deadline_in_ms(0u) == Adapter::no_deadline,
		      "idle scheduler sees no protocol deadline");
		auto message = make(endpoint);
		check(message.append_bytes(data) && endpoint.send(message) == wire::SendResult::Sent,
		      "one common builder/send sequence starts UART DMA");
		frame.assign(fake::model().tx_src, fake::model().tx_src + fake::model().tx_len);
		check(serial.tx_busy() && endpoint.tx_active() && !message, "DMA borrows endpoint TX storage");
		{
			Adapter refused{serial, endpoint};
			check(!refused.bind() && !refused.bound() && !adapter.unbind(), "active TX prevents rebinding/detaching");
		} // an unsuccessfully bound adapter must not detach the original's handlers
		fake::rx_bytes(frame.data(), frame.size());
		fake::rx_idle();
		check(!endpoint.has_packet() && fake_freertos::model().last_notified == task &&
		      uart::FreeRtosWake::wait(adapter, 0u) == 1u &&
		      fake_freertos::model().last_take_timeout == 50u,
		      "RX ISR only wakes the task; common wait/proceed loop parses later");
		adapter.proceed(0u);
		check(endpoint.has_packet() && sentinel.chunks == 1u, "failed second bind/destructor preserves first adapter");
		fake::tx_done();
		check(uart::FreeRtosWake::wait(50u) == 1u, "TX completion wakes either protocol's task");
		check(endpoint.tx_active() && !adapter.unbind(), "completed DMA still awaits endpoint poll");
		adapter.proceed(1u);
		check(!endpoint.tx_active() && endpoint.storage().tx_available() == 2u,
		      "proceed drains driver then releases completed TX block");
		auto packet = endpoint.pop_packet();
		check(packet && std::ranges::equal(packet.data(), data), "Packet data matches across both adapters");
		packet.reset();
		const auto before_gap = endpoint.stats();
		fake::rx_bytes(frame.data(), 3u);
		fake::rx_error(HAL_UART_ERROR_ORE);
		adapter.proceed(2u);
		check(!endpoint.has_packet() && !adapter.deadline_armed() &&
		      endpoint.stats().rx.frames_received == before_gap.rx.frames_received,
		      "UART error is an ordered gap, never a partial Packet");
		if constexpr (requires { E::length_size; }) {
			check(endpoint.stats().rx.frames_lost == before_gap.rx.frames_lost + 1u,
			      "COBS adapter forwards the actual driver's gap callback exactly once");
			const uint8_t delimiter = 0u;
			fake::rx_bytes(&delimiter, 1u);
			fake::rx_idle();
			adapter.proceed(2u);
		} else {
			check(endpoint.stats().rx.stream_gaps == before_gap.rx.stream_gaps + 1u,
			      "RTU adapter forwards the actual driver's gap callback exactly once");
		}

		if constexpr (requires { E::length_size; }) {
			// Maximum default COBS frames cross the Uart<256,4> DMA boundary.
			const std::vector<uint8_t> large(E::max_send_size, 0xA5u);
			message = endpoint.make_message(large.size());
			check(message.append_bytes(large) && endpoint.send(message) == wire::SendResult::Sent,
			      "maximum COBS payload starts DMA through the new adapter");
			const std::vector<uint8_t> full(fake::model().tx_src,
				fake::model().tx_src + fake::model().tx_len);
			check(full.size() > 256u, "maximum test frame crosses a full DMA chunk");
			fake::tx_done();
			adapter.proceed(2u);
			fake::rx_bytes(full.data(), 256u);
			fake::rx_tc();
			adapter.proceed(2u);
			check(!endpoint.has_packet(), "DMA TC fragment is not a complete COBS packet");
			// Deliver the tail bytewise: arbitrary IDLE cuts, delimiter last.
			for (std::size_t i = 256u; i < full.size(); ++i) {
				fake::rx_bytes(full.data() + i, 1u);
				fake::rx_idle();
				adapter.proceed(2u);
			}
			packet = endpoint.pop_packet();
			check(packet && std::ranges::equal(packet.data(), large), "full DMA chunk plus split IDLE tail round trips");
			packet.reset();
		}

		// Preserve a ready packet while detaching; discard any partial stream
		// assembly, with the synchronization boundary appropriate to the protocol.
		fake::rx_bytes(frame.data(), frame.size());
		fake::rx_idle();
		adapter.proceed(2u);
		if constexpr (requires { endpoint.consume(std::span<const uint8_t>{}); }) {
			fake::rx_bytes(frame.data(), 3u);
			fake::rx_idle();
			adapter.proceed(2u);
		}
		check(adapter.unbind() && !adapter.bound() && adapter.unbind(), "unbind is successful and idempotent");
		check(endpoint.has_packet() && endpoint.storage().rx_available() == 1u,
		      "detach preserves one ready packet and releases partial RX storage");
		packet = endpoint.pop_packet();
		check(packet && std::ranges::equal(packet.data(), data), "queued Packet survives detach");
		packet.reset();
		fake::rx_bytes(frame.data(), frame.size());
		fake::rx_idle();
		adapter.proceed(3u);
		check(!endpoint.has_packet(), "unbound driver never calls detached adapter");
		check(adapter.bind(), "rebind succeeds");
		if constexpr (requires { E::length_size; }) {
			const auto before = endpoint.stats().rx.frames_received;
			fake::rx_bytes(frame.data(), frame.size());
			fake::rx_idle();
			adapter.proceed(4u);
			check(!endpoint.has_packet() && endpoint.stats().rx.frames_received == before,
			      "COBS detach hunts through the first new delimiter, never gluing an old partial");
		}
		fake::rx_bytes(frame.data(), frame.size());
		fake::rx_idle();
		adapter.proceed(4u);
		packet = endpoint.pop_packet();
		check(packet && std::ranges::equal(packet.data(), data), "new frame accepted after synchronization");
		packet.reset();
		message = make(endpoint);
		check(message.append_bytes(data) && endpoint.send(message) == wire::SendResult::Sent,
		      "TX begins before bound adapter destruction");
	} // safe to destroy only the adapter: its TX delegates target the live driver
	check(serial.tx_busy() && endpoint.tx_active(), "adapter destructor does not free DMA-borrowed TX");
	fake::rx_bytes(frame.data(), frame.size());
	fake::rx_idle();
	serial.proceed(5u);
	check(!endpoint.has_packet(), "adapter destructor detaches RX callbacks");
	fake::tx_done();
	serial.proceed(6u);
	endpoint.poll(6u);
	check(!endpoint.tx_active() && endpoint.unbind() && endpoint.storage().tx_available() == 2u,
	      "live driver/endpoint safely finish TX after adapter destruction");
	check(fake::model().violations.empty(), "no DMA ownership violation");
}
} // namespace

int main()
{
	using Memory = wire::Pool<2u, 2u>;
	exercise<cobs::Endpoint<Memory>, cobs::UartAdapter>("COBS");
	exercise<cobs::Endpoint<Memory, cobs::Format<crc::Crc16Table>>, cobs::UartAdapter>("COBS/Table");
	exercise<cobs::Endpoint<Memory, cobs::Format<crc::NoCrc>>, cobs::UartAdapter>("COBS/NoCrc");
	exercise<modbus::rtu::Endpoint<Memory>, modbus::rtu::UartAdapter>("RTU/burst");
	exercise<modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>,
		modbus::rtu::framing::Standard<modbus::rtu::framing::Direction::Request>>,
		modbus::rtu::UartAdapter>("RTU/framed");
	std::printf("UART adapter parity: %u checks, %u failures\n", checks, failures);
	return failures == 0u ? 0 : 1;
}
