// INTEGRATION.md §4 verbatim on the recording FreeRTOS fake; xTaskCreate is
// stubbed here because the fake models only the notification API.
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"
#include "modbus/rtu/Rtu.h"
#include "modbus/rtu/UartAdapter.h"
#include "FreeRtosWake.h"
#include "platform_fake.h"
#include <algorithm>
#include "Test.h"
#include <cstdio>

namespace framing = modbus::rtu::framing;
using Serial = Uart<256, 4>;
using Server = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
                                     framing::Standard<framing::Direction::Request>>;
static Serial serial;
static Server link;
static modbus::rtu::UartAdapter adapter{serial, link};

// --- what the fake FreeRTOS does not model: task creation ---
using TaskFunction_t = void (*)(void*);
#define pdPASS 1
static tskTaskControlBlock* const kFakeTask = reinterpret_cast<tskTaskControlBlock*>(0x20001000u);
static BaseType_t xTaskCreate(TaskFunction_t, const char*, uint16_t, void*, UBaseType_t, TaskHandle_t* out)
{
	*out = kFakeTask;
	return pdPASS;
}
static unsigned g_served = 0;
static void serve(const Server::Packet&) noexcept { ++g_served; }
// ------------------------------------------------------------

static uart::FreeRtosWake wake;                    // takes no task: safe at static-init time
static TaskHandle_t comm_task = nullptr;

static void comm_task_iteration() noexcept        // one pass of the for(;;) body below
{
	const uint32_t now = HAL_GetTick();
	(void)uart::FreeRtosWake::wait(std::min(50u, adapter.deadline_in_ms(now)));
	adapter.proceed(HAL_GetTick());
	while (auto request = link.pop_packet()) {
		serve(request);
	}
}

void comm_task_body(void*)
{
	for (;;) {
		comm_task_iteration();
	}
}

bool start_comm() noexcept
{
	if (xTaskCreate(comm_task_body, "comm", 512, nullptr, 3, &comm_task) != pdPASS) {
		return false;
	}
	// After the handle exists. A null handle is refused and nothing is installed.
	return wake.attach(serial, comm_task);
}

int main()
{
	fake::reset();
	fake_freertos::reset();
	configure_huart3(115200u);
	if (!(serial.init(&huart3) && adapter.bind() && start_comm())) { std::puts("start failed"); return 1; }
	const bool null_refused = !uart::FreeRtosWake{}.attach(serial, nullptr);
	const auto adu = modbus_test::make_adu(0x11u, 0x03u, std::vector<uint8_t>{0x00u, 0x6Bu, 0x00u, 0x03u});
	fake::rx_bytes(adu.data(), adu.size());
	fake::rx_idle();
	const auto& rtos = fake_freertos::model();
	const bool notified = rtos.notifications_from_isr == 1u && rtos.last_notified == kFakeTask;
	comm_task_iteration();
	const bool ok = null_refused && notified && g_served == 1u && rtos.last_take_timeout == 50u &&
	                fake::model().violations.empty();
	std::printf("freertos_wake: notified=%d served=%u wait=%u -> %s\n", notified, g_served,
	            static_cast<unsigned>(rtos.last_take_timeout), ok ? "ok" : "FAIL");
	return ok ? 0 : 1;
}
