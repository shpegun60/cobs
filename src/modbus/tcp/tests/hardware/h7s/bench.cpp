/* Author: shpegun60; SPDX-License-Identifier: MIT */
// MBAP over UART is a core/MCU test transport, NOT Ethernet/TCP/IP coverage.
#define UART_ENGINE_IMPLEMENT
#include "uart/Uart.h"
#include "uart_bench.h"
#include "usart.h"
#include "modbus/tcp/tests/core_cases.h"
#include "modbus/rtu/Rtu.h"
#include <cstdio>
#include <cstdlib>

#ifndef TCP_HW_HEAP
#define TCP_HW_HEAP 0
#endif
#ifndef TCP_HW_CRC
#define TCP_HW_CRC 0
#endif
BenchCounter g_bench_usart_irq{}, g_bench_rx_dma_irq{}, g_bench_tx_dma_irq{};

namespace {
using Integrity = std::conditional_t<TCP_HW_CRC == 0, crc::NoCrc,
	std::conditional_t<TCP_HW_CRC == 1, crc::Crc16Bitwise,
	std::conditional_t<TCP_HW_CRC == 2, crc::Crc16Table, crc::Crc32Table>>>;
using Memory = std::conditional_t<TCP_HW_HEAP != 0, wire::Heap, wire::Pool<8, 2>>;
using Link = modbus::tcp::Endpoint<Memory, modbus::tcp::Format<Integrity, 1024u>>;
using Serial = Uart<256, 4>;
// UART DMA-readable AXI SRAM, never stack/DTCM for Pool TX.
Link endpoint;
Serial driver;
bool echo_mode = false;
Link::Message pending;
std::array<void*, 1024u> pressure{};

void emit(const char* text) noexcept
{
	if (HAL_UART_Transmit(&huart3, reinterpret_cast<const uint8_t*>(text),
		static_cast<uint16_t>(std::strlen(text)), 2000u) != HAL_OK) { Error_Handler(); }
}

tcp_test::Result oom_cases() noexcept
{
	tcp_test::Result result;
	auto check = [&result](bool okay, unsigned line) noexcept {
		++result.checks;
		if (!okay && result.failed_line == 0u) { result.failed_line = line; }
	};
#define REQUIRE(...) check(static_cast<bool>(__VA_ARGS__), __LINE__)
	{
		modbus::tcp::Endpoint<wire::Pool<1, 1>> pool;
		const uint8_t frame[]{0, 1, 0, 0, 0, 2, 1, 3};
		pool.consume(frame);
		auto held = pool.pop_packet();
		pool.consume(frame);
		REQUIRE(held && pool.stats().rx.allocation_failure == 1u && pool.stats().rx.skipped_frames == 1u && !pool.has_packet());
		held.reset();
		pool.consume(frame);
		REQUIRE(pool.pop_packet());
	}
	if constexpr (TCP_HW_HEAP != 0) {
		modbus::tcp::Endpoint<wire::Heap, modbus::tcp::Format<Integrity>> heap;
		tcp_test::Capture capture;
		REQUIRE(tcp_test::bind(heap, capture));
		auto message = heap.make_message(0x1234u, 2u, 3u, 2u);
		REQUIRE(message && message.append_be(uint16_t{0xABCDu}));
		std::array<uint8_t, 32u> input{};
		const uint8_t payload[]{0xABu, 0xCDu};
		const auto total = tcp_test::frame<Integrity>(input.data(), 0x1234u, 2u, 3u, payload);
		std::size_t held_count = 0u;
		unsigned refused = 0u;
		for (const std::size_t size : {4096u, 256u, 16u, 1u}) {
			while (held_count < pressure.size()) {
				void* const p = std::malloc(size);
				if (p == nullptr) { ++refused; break; }
				pressure[held_count++] = p;
			}
		}
		REQUIRE(refused == 4u && held_count < pressure.size());
		REQUIRE(!heap.make_message(1u, 2u, 3u, 32u));
		REQUIRE(!message.reserve(64u) && !message.append_be(uint32_t{0x11223344u}));
		REQUIRE(message.size() == 2u && message.capacity() == 2u);
		heap.consume({input.data(), 6u});
		REQUIRE(heap.stats().rx.allocation_failure == 1u && heap.assembling());
		while (held_count != 0u) { std::free(pressure[--held_count]); pressure[held_count] = nullptr; }
		heap.consume({input.data() + 6u, total - 6u});
		REQUIRE(!heap.has_packet() && !heap.assembling() && heap.stats().rx.skipped_frames == 1u);
		REQUIRE(message.reserve(64u) && message.append_be(uint16_t{0x1234u}));
		REQUIRE(heap.send(message) == wire::SendResult::Sent);
		heap.consume(capture.view());
		auto packet = heap.pop_packet();
		REQUIRE(packet && packet.size() == 4u);
		if (packet && packet.size() == 4u) {
			REQUIRE(packet.data()[0] == 0xABu && packet.data()[1] == 0xCDu && packet.data()[2] == 0x12u && packet.data()[3] == 0x34u);
		}
		capture.active = false;
		heap.poll(0u);
	}
#undef REQUIRE
	return result;
}

tcp_test::Result rtu_data_cases() noexcept
{
	tcp_test::Result result;
	using Rtu = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<Integrity, 1024u>>;
	static_assert(Rtu::max_send_size == 1024u && Rtu::max_receive_size == 1024u);
	static_assert(Rtu::max_frame_size == 1026u + Integrity::wire_size);
	static_assert(modbus::rtu::Endpoint<>::max_frame_size == 256u);
	Rtu rtu;
	tcp_test::Capture capture;
	auto check = [&result](bool okay, unsigned line) noexcept {
		++result.checks;
		if (!okay && result.failed_line == 0u) { result.failed_line = line; }
	};
#define REQUIRE(...) check(static_cast<bool>(__VA_ARGS__), __LINE__)
	REQUIRE(tcp_test::bind(rtu, capture));
	std::array<uint8_t, 1024u> data{};
	for (std::size_t i = 0u; i < data.size(); ++i) { data[i] = static_cast<uint8_t>(i * 73u); }
	std::array<uint8_t, Rtu::max_frame_size> expected{};
	for (const std::size_t size : {0u, 1u, 7u, 252u, 255u, 256u, 1024u}) {
		auto message = rtu.make_message(0x11u, 0x67u, 0u);
		REQUIRE(message && message.append_bytes({data.data(), size}));
		REQUIRE(message.size() == size);
		const auto total = 2u + size + Integrity::wire_size;
		expected[0] = 0x11u; expected[1] = 0x67u;
		std::copy_n(data.begin(), size, expected.begin() + 2u);
		tcp_test::trailer<Integrity>({expected.data(), size + 2u}, expected.data() + size + 2u);
		REQUIRE(rtu.send(message) == wire::SendResult::Sent);
		REQUIRE(capture.size == total && std::memcmp(capture.bytes.data(), expected.data(), total) == 0);
		rtu.receive_adu({expected.data(), total});
		auto packet = rtu.pop_packet();
		REQUIRE(packet && packet.size() == size && packet.adu().size() == total);
		if (packet) { REQUIRE(std::equal(packet.data().begin(), packet.data().end(), data.begin())); }
		capture.active = false; rtu.poll(0u);
		REQUIRE(!rtu.tx_active());
	}
	auto maximum = rtu.make_message(1u, 3u, 1024u);
	REQUIRE(maximum && maximum.capacity() == 1024u && maximum.append_bytes(data));
	REQUIRE(!maximum.append_native(uint8_t{0u}) && maximum.size() == 1024u);
	REQUIRE(!rtu.make_message(1u, 3u, 1025u));
#undef REQUIRE
	return result;
}

void start_echo() noexcept
{
	driver.setRxHandler(Serial::RxHandler{[](std::span<const uint8_t> bytes) noexcept { endpoint.consume(bytes); }});
	driver.setRxGapHandler(Serial::GapHandler{[]() noexcept { endpoint.notify_gap(); }});
	if (!endpoint.bind(Link::Sender{tiny::bind<&Serial::send>(driver)},
		Link::BusyQuery{tiny::bind<&Serial::tx_busy>(driver)}) || !driver.init(&huart3)) { Error_Handler(); }
	// The runner sends no ADU until this acknowledgement is completely received.
	emit("ECHO,115200,DATA,1024\n");
	echo_mode = true;
}
} // namespace

extern "C" void bench_init()
{
	SCB_EnableICache(); SCB_EnableDCache();
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	*reinterpret_cast<volatile uint32_t*>(DWT_BASE + 0xFB0u) = 0xC5ACCE55u;
	DWT->CYCCNT = 0u; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	huart3.Init.BaudRate = 115200u;
	if (HAL_UART_Init(&huart3) != HAL_OK) { Error_Handler(); }
}
extern "C" void bench_loop()
{
	if (echo_mode) {
		const auto now = HAL_GetTick();
		driver.proceed(now);
		endpoint.poll(now);
		if (endpoint.rx_failed()) { return; } // no scanner; reset MCU = new test stream
		if (!pending && !endpoint.tx_active() && endpoint.has_packet()) {
			auto packet = endpoint.pop_packet();
			pending = endpoint.make_message(packet.transaction_id(), packet.unit_id(), packet.function(), packet.size());
			if (!pending || !pending.append_bytes(packet.data())) { Error_Handler(); }
		}
		if (pending) {
			const auto status = endpoint.send(pending);
			if (status != wire::SendResult::Sent && status != wire::SendResult::Busy) { Error_Handler(); }
		}
		return;
	}
	uint8_t command = 0u;
	if (HAL_UART_Receive(&huart3, &command, 1u, 1u) != HAL_OK) { return; }
	if (command == 'H') {
		char line[128];
		(void)std::snprintf(line, sizeof(line), "TCP,2,%lu,%u,%u,1024,%lu,%u\n",
			static_cast<unsigned long>(SystemCoreClock), TCP_HW_HEAP, TCP_HW_CRC,
			static_cast<unsigned long>(SCB->CCR), static_cast<unsigned>(Link::max_frame_size));
		emit(line);
	} else if (command == 'S') {
		const auto core = tcp_test::core_cases<Memory, Integrity>();
		const auto oom = oom_cases();
		const auto rtu = rtu_data_cases();
		char line[128];
		(void)std::snprintf(line, sizeof(line), "SELF,%u,%u,%u,%u,%u,%u\n", core.checks, core.failed_line,
			oom.checks, oom.failed_line, rtu.checks, rtu.failed_line);
		emit(line);
	} else if (command == 'E') { start_echo(); }
}
