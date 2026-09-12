/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Live FreeRTOS, real HAL/UART/DMA. No fake headers or heap implementation.
#define UART_ENGINE_IMPLEMENT
#include "uart/Uart.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/cobs/UartAdapter.h"
#include "adapters/rtu/UartAdapter.h"
#include "adapters/freertos/FreeRtosWake.h"
#include "wire/tests/contract_checks.h"
#include "uart_bench.h"
#include "usart.h"

#include <algorithm>
#include <array>
#include <span>

#ifndef PARITY_PROTOCOL
#define PARITY_PROTOCOL 0
#endif
#ifndef PARITY_CRC
#define PARITY_CRC 1
#endif
#ifndef PARITY_BAUD
#define PARITY_BAUD 115200u
#endif

BenchCounter g_bench_usart_irq, g_bench_rx_dma_irq, g_bench_tx_dma_irq;
extern "C" {
volatile uint32_t g_parity_assertions = 0u;
volatile uint32_t g_parity_isr_notifies = 0u;
volatile uint32_t g_parity_bad_notify_context = 0u;
volatile uint32_t g_parity_idle = 0u;
void xPortSysTickHandler(void);
void parity_assert_failed(void) { g_parity_assertions = g_parity_assertions + 1u; __disable_irq(); for (;;) {} }
void parity_trace_notify(void)
{
	g_parity_isr_notifies = g_parity_isr_notifies + 1u;
	if (__get_IPSR() == 0u) { g_parity_bad_notify_context = g_parity_bad_notify_context + 1u; }
}
void vApplicationIdleHook(void) { g_parity_idle = g_parity_idle + 1u; __WFI(); }
void vApplicationStackOverflowHook(TaskHandle_t, char*) { parity_assert_failed(); }
void vApplicationGetIdleTaskMemory(StaticTask_t** tcb, StackType_t** stack, uint32_t* size)
{
	static StaticTask_t idle_tcb;
	static StackType_t idle_stack[configMINIMAL_STACK_SIZE];
	*tcb = &idle_tcb; *stack = idle_stack; *size = configMINIMAL_STACK_SIZE;
}
void SysTick_Handler(void)
{
	HAL_IncTick();
	if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) { xPortSysTickHandler(); }
}
}

namespace {
#if PARITY_CRC == 0
using Integrity = crc::NoCrc;
#elif PARITY_CRC == 1
using Integrity = crc::Crc16Bitwise;
#elif PARITY_CRC == 2
using Integrity = crc::Crc16Table;
#else
#error invalid PARITY_CRC
#endif
namespace framing = modbus::rtu::framing;
struct PrivateFramer : framing::Standard<framing::Direction::Request> {
	static constexpr framing::Layout layout(framing::Direction d, uint8_t fn) noexcept
	{
		return fn == 0x41u ? framing::Layout::length_prefixed(2u) : framing::standard_layout(d, fn);
	}
};
using Memory = wire::Pool<4u, 2u>;
using Serial = Uart<256u, 4u>;
#if PARITY_PROTOCOL == 0
using Link = cobs::Endpoint<Memory, cobs::Format<Integrity>>;
using Adapter = cobs::UartAdapter<Serial, Link>;
#elif PARITY_PROTOCOL == 1
using Link = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<Integrity, 254u - Integrity::wire_size>>;
using Adapter = modbus::rtu::UartAdapter<Serial, Link>;
#elif PARITY_PROTOCOL == 2
using Link = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<Integrity, 254u - Integrity::wire_size>, PrivateFramer>;
using Adapter = modbus::rtu::UartAdapter<Serial, Link>;
#else
#error invalid PARITY_PROTOCOL
#endif
static_assert(std::same_as<cobs::SendResult, modbus::rtu::SendResult>);
using Reader = bool (*)(std::span<const uint8_t>, std::size_t&, uint16_t&) noexcept;
static_assert(static_cast<Reader>(&cobs::read_be<uint16_t>) == &modbus::rtu::read_be<uint16_t>);

uart::FreeRtosWake wake;
Serial serial;
Link endpoint;
Adapter adapter{serial, endpoint};
StaticTask_t task_tcb;
StackType_t task_stack[4096u];
uint32_t checks = 0u, failed = 0u, notified = 0u, timed_out = 0u, echoes = 0u;
uint32_t bad_task_context = 0u, local_checks = 0u, local_failed = 0u;
uint32_t detach_before = 0u, detach_after = 0u;
Link::Packet retained;
bool detaching = false;
constexpr std::array<uint8_t, 4> magic{0xB6u, 0x50u, 0x52u, 0x54u};
void check(bool condition) { ++checks; if (!condition) { ++failed; } }

Link::Message make(Link& link, std::size_t body_size)
{
#if PARITY_PROTOCOL == 0
	return link.make_message(body_size);
#else
	auto message = link.make_message(0x11u, 0x41u, body_size + 2u);
#if PARITY_PROTOCOL == 1
	if (!message.append_be(static_cast<uint16_t>(body_size))) { return {}; }
#endif
	return message;
#endif
}
std::span<const uint8_t> body_of(const Link::Packet& packet)
{
#if PARITY_PROTOCOL == 0
	return packet.data();
#else
	std::size_t offset = 0u;
	uint16_t length = 0u;
	std::span<const uint8_t> body;
	if (!modbus::rtu::read_be(packet.data(), offset, length) ||
	    !modbus::rtu::read_bytes(packet.data(), offset, length, body) || offset != packet.size()) {
		++failed; return {};
	}
	return body;
#endif
}
void receive_local(Link& link, std::span<const uint8_t> frame)
{
#if PARITY_PROTOCOL == 1
	link.receive_adu(frame);
#else
	for (const auto& byte : frame) { link.consume(std::span<const uint8_t>{&byte, 1u}); }
#endif
}
struct Capture {
	std::array<uint8_t, 300u> bytes{};
	std::size_t size = 0u;
	bool busy = false, accept = false;
	bool send(std::span<const uint8_t> data) noexcept
	{
		if (data.size() > bytes.size()) { return false; }
		std::copy(data.begin(), data.end(), bytes.begin()); size = data.size();
		busy = accept; return accept;
	}
	bool occupied() const noexcept { return busy; }
};
void delayed_notification(void* target)
{
	for (;;) {
		(void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
		vTaskDelay(10u);
		xTaskNotifyGive(static_cast<TaskHandle_t>(target));
	}
}
void wait_contract()
{
	static_assert(configTICK_RATE_HZ == 1000u && sizeof(TickType_t) == 4u);
	for (const uint32_t ms : std::array<uint32_t, 13u>{0u, 1u, 5u, 50u, 1000u, 65534u, 65535u, 65536u,
	                         4294967u, 4294968u, 7200000u, UINT32_MAX - 1u, UINT32_MAX}) {
		check(uart::detail::wake_ticks_for(ms) == (ms == UINT32_MAX ? UINT32_MAX - 1u : ms));
	}
	static StaticTask_t notifier_tcb;
	static StackType_t notifier_stack[512u];
	const auto notifier = xTaskCreateStatic(delayed_notification, "wait-check", 512u,
		xTaskGetCurrentTaskHandle(), 2u, notifier_stack, &notifier_tcb);
	check(notifier != nullptr);
	if (notifier == nullptr) { return; }
	for (const uint32_t ms : std::array<uint32_t, 2u>{4294968u, UINT32_MAX}) {
		(void)ulTaskNotifyTake(pdTRUE, 0u);
		const auto started = xTaskGetTickCount();
		xTaskNotifyGive(notifier);
		const auto taken = uart::FreeRtosWake::wait(ms);
		const auto elapsed = xTaskGetTickCount() - started;
		check(taken == 1u && elapsed >= 10u && elapsed < 100u);
	}
}
void local_contract()
{
	const auto before_checks = checks, before_failed = failed;
	Capture capture;
	Link link, foreign;
	constexpr std::array<uint8_t, 4> payload{0x12u, 0x34u, 0x78u, 0x56u};
	Link::Message empty;
	check(link.send(empty) == wire::SendResult::Invalid);
	auto message = make(link, payload.size());
	check(bool(message));
	check(message.append_be(uint16_t{0x1234u}) && message.append_le(uint16_t{0x5678u}));
	const auto size = message.size(), capacity = message.capacity();
	check(!message.reserve(Link::max_send_size + 1u) && message.size() == size && message.capacity() == capacity);
	check(link.send(message) == wire::SendResult::Unbound);
	check(foreign.send(message) == wire::SendResult::Invalid);
	check(link.bind(Link::Sender{tiny::bind<&Capture::send>(capture)},
	                Link::BusyQuery{tiny::bind<&Capture::occupied>(capture)}));
	capture.busy = true;
	check(link.send(message) == wire::SendResult::Busy && message.reserve(capacity));
	capture.busy = false;
	check(link.send(message) == wire::SendResult::Failed && bool(message));
	const auto saved = capture.bytes;
	check(!message.append_native(uint8_t{0u}) && !message.reserve(capacity));
	capture.accept = true;
	check(link.send(message) == wire::SendResult::Sent && !message && link.tx_active());
	check(capture.bytes == saved && !link.unbind());
	receive_local(link, std::span<const uint8_t>{capture.bytes.data(), capture.size});
	auto packet = link.pop_packet();
	check(packet && std::ranges::equal(body_of(packet), payload));
	auto held = packet; packet.reset();
	check(!packet && held && std::ranges::equal(body_of(held), payload));
	std::size_t offset = 0u; uint16_t a = 0u, b = 0u;
	check(cobs::read_be(body_of(held), offset, a) && modbus::rtu::read_le(body_of(held), offset, b) &&
	      a == 0x1234u && b == 0x5678u);
	check(!wire::read_be(body_of(held), offset, a) && offset == 4u && a == 0x1234u);
	capture.busy = false;
	check(link.tx_active() && !link.unbind());
	link.poll(HAL_GetTick());
	check(!link.tx_active() && link.unbind() && link.storage().tx_available() == 2u);
	check(link.stats().rx.frames_received == 1u && link.stats().tx.frames_sent == 1u && link.stats().tx.send_failed == 1u);
	held.reset();
	check(link.storage().rx_available() == 4u);
	contract_checks::readers(check);
	contract_checks::policies<wire::Pool<2u, 2u>>(check);
	wait_contract();
	local_checks = checks - before_checks; local_failed = failed - before_failed;
}
bool send_body(std::span<const uint8_t> body)
{
	auto message = make(endpoint, body.size());
	return message && message.append_bytes(body) && endpoint.send(message) == wire::SendResult::Sent;
}
void status(uint8_t command)
{
	// Stable 24-word telemetry. Values are a thread-context snapshot, not a CPU benchmark.
	const auto uart = serial.stats();
	const std::array<uint32_t, 24u> words{
		2u, PARITY_PROTOCOL, PARITY_CRC, SystemCoreClock, PARITY_BAUD,
		10000u * tskKERNEL_VERSION_MAJOR + 100u * tskKERNEL_VERSION_MINOR + tskKERNEL_VERSION_BUILD,
		checks, failed, local_checks, local_failed, g_parity_assertions,
		g_parity_isr_notifies, g_parity_bad_notify_context, notified, timed_out,
		g_parity_idle, bad_task_context, echoes, endpoint.stats().rx.frames_received,
		static_cast<uint32_t>(endpoint.storage().rx_available()),
		static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)),
		detach_before, detach_after, uart.restarts};
	auto message = make(endpoint, magic.size() + 1u + sizeof(words));
	check(message && message.append_bytes(magic) && message.append_native(command) &&
	      message.append_le(std::span<const uint32_t>{words}) && endpoint.send(message) == wire::SendResult::Sent);
}
void communication(void*)
{
	check(__get_IPSR() == 0u && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING);
	check(serial.init(&huart3));
	HAL_NVIC_SetPriority(USART3_IRQn, 6u, 0u);
	HAL_NVIC_SetPriority(GPDMA1_Channel10_IRQn, 6u, 0u);
	HAL_NVIC_SetPriority(GPDMA1_Channel11_IRQn, 6u, 0u);
	check(adapter.bind());
	check(!wake.attach(serial, nullptr));
	check(wake.attach(serial, xTaskGetCurrentTaskHandle()));
	local_contract();
	for (;;) {
		if (uart::FreeRtosWake::wait(adapter) != 0u) { ++notified; }
		else { ++timed_out; }
		if (__get_IPSR() != 0u) { ++bad_task_context; }
		adapter.proceed();
		if (detaching && !endpoint.tx_active() && endpoint.storage().rx_available() < 3u) {
			detach_before = static_cast<uint32_t>(endpoint.storage().rx_available());
			check(adapter.unbind() && !adapter.bound());
			detach_after = static_cast<uint32_t>(endpoint.storage().rx_available());
			check(detach_before == 2u && detach_after == 3u && bool(retained));
			check(adapter.bind() && adapter.bound());
			retained.reset();
			check(endpoint.storage().rx_available() == 4u);
			detaching = false;
			status('d');
		}
		if (endpoint.tx_active()) { continue; }
		auto packet = endpoint.pop_packet();
		if (!packet) { continue; }
		const auto body = body_of(packet);
		if (body.size() == 5u && std::equal(magic.begin(), magic.end(), body.begin())) {
			const uint8_t command = body[4];
			if (command == 'D') { retained = packet; detaching = true; }
			status(command);
			if (command == 'B') {
				check(endpoint.tx_active() && !adapter.unbind());
				Adapter other{serial, endpoint};
				check(!other.bind() && !other.bound());
			}
		} else { check(send_body(body)); ++echoes; }
	}
}
} // namespace

extern "C" void bench_init(void)
{
	SCB_EnableICache(); SCB_EnableDCache();
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0u; DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	huart3.Init.BaudRate = PARITY_BAUD;
	if (HAL_UART_Init(&huart3) != HAL_OK || HAL_UARTEx_EnableFifoMode(&huart3) != HAL_OK) { Error_Handler(); }
	if (xTaskCreateStatic(communication, "wire", 4096u, nullptr, 3u, task_stack, &task_tcb) == nullptr) { Error_Handler(); }
	vTaskStartScheduler();
	Error_Handler();
}
extern "C" void bench_loop(void) {}
