/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Copyable task translation unit. Define UART_ENGINE_IMPLEMENT in exactly ONE
// firmware TU. The application supplies an initialized huart3 and kernel.
#define UART_ENGINE_IMPLEMENT
#include "uart/Uart.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/cobs/UartAdapter.h"
#include "adapters/rtu/UartAdapter.h"
#include "adapters/freertos/FreeRtosWake.h"
#include "FreeRTOS.h"
#include "task.h"

#ifndef DOC_RTU
#define DOC_RTU 0
#endif
#ifdef DOC_HOST
#include "platform_fake.h"
#include "Example.h"
// Test-only task creation scaffold: no host scheduler is simulated.
struct StaticTask_t {};
using StackType_t = uint32_t;
static constexpr UBaseType_t tskIDLE_PRIORITY = 0;
static TaskHandle_t xTaskGetCurrentTaskHandle() { return reinterpret_cast<TaskHandle_t>(0x20001000u); }
static TaskHandle_t xTaskCreateStatic(void (*)(void*), const char*, uint32_t,
    void*, UBaseType_t, StackType_t*, StaticTask_t*) { return xTaskGetCurrentTaskHandle(); }
static void vTaskSuspend(TaskHandle_t) { std::abort(); }
#else
extern UART_HandleTypeDef huart3;
#endif

using Serial = Uart<256, 4>;
#if DOC_RTU
namespace framing = modbus::rtu::framing;
using Link = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Request>>;
using Adapter = modbus::rtu::UartAdapter<Serial, Link>;
#else
using Link = cobs::Endpoint<wire::Pool<8, 2>>;
using Adapter = cobs::UartAdapter<Serial, Link>;
#endif

// Serial and Pool TX backing memory must be DMA-readable. Static lifetime
// alone is NOT a linker/MPU placement guarantee.
static Serial serial;
static Link g_endpoint;
static Adapter adapter{serial, g_endpoint};
static uart::FreeRtosWake wake;
static Link::Message pending;
static unsigned dropped = 0;

// example-begin: freertos-start
static bool communication_start()
{
    // Executed BY the task, not by its creator after it can already run.
    return wake.attach(serial, xTaskGetCurrentTaskHandle()) &&
           serial.init(&huart3) && adapter.bind();
}
// example-end: freertos-start

// example-begin: freertos-service
static void communication_step()
{
    (void)uart::FreeRtosWake::wait(adapter);
    adapter.proceed(); // fresh HAL tick, UART RX/gap, parser, TX reclamation
    if (!pending) {
        if (auto packet = g_endpoint.pop_packet()) {
#if DOC_RTU
            // Small demo: unit 1, FC03, register 0, count 1; not a register server.
            std::size_t offset = 0;
            uint16_t start = 0, count = 0;
            if (packet.address() != 1u || packet.function() != 3u ||
                !wire::read_be(packet.data(), offset, start) ||
                !wire::read_be(packet.data(), offset, count) ||
                offset != packet.size() || start != 0u || count != 1u) { ++dropped; return; }
            pending = g_endpoint.make_message(packet.address(), packet.function());
            const bool built = pending && pending.append_be(uint8_t{2}) && pending.append_be(uint16_t{100});
#else
            pending = g_endpoint.make_message(packet.size());
            const bool built = pending && pending.append_bytes(packet.data());
#endif
            if (!built) { pending = {}; ++dropped; }
        }
    }
    if (pending) {
        const auto result = g_endpoint.send(pending);
        if (result != wire::SendResult::Sent && result != wire::SendResult::Busy) {
            pending = {}; ++dropped; // explicit policy; no automatic physical-error retries
        }
    }
}
// example-end: freertos-service

// example-begin: freertos-task
static void communication_task(void*)
{
    if (!communication_start()) {
        for (;;) { vTaskSuspend(nullptr); } // replace with your fatal-error policy
    }
    for (;;) { communication_step(); }
}

TaskHandle_t start_communication_task()
{
    static StaticTask_t control;
    static StackType_t stack[768]; // ELEMENTS, not bytes; measure your real high-water mark
    return xTaskCreateStatic(communication_task, "comm", 768u, nullptr,
                             tskIDLE_PRIORITY + 2u, stack, &control);
}
// example-end: freertos-task

#ifdef DOC_HOST
int main()
{
    fake::reset();
    fake_freertos::reset();
    configure_huart3(115200u);
    CHECK(start_communication_task() != nullptr && communication_start());
    example::Transport capture;
#if DOC_RTU
    modbus::rtu::Endpoint<> peer;
    auto message = peer.make_message(1u, 3u);
    CHECK(message && message.append_be(uint16_t{0}) && message.append_be(uint16_t{1}));
#else
    cobs::Endpoint<> peer;
    auto message = peer.make_message();
    CHECK(message && message.append_be(uint16_t{1234}));
#endif
    CHECK(example::bind(peer, capture) && peer.send(message) == wire::SendResult::Sent);
    peer.poll(0u);
    fake::rx_bytes(capture.frame().data(), capture.frame().size());
    fake::rx_idle();
    CHECK(!g_endpoint.has_packet()); // parsing did not run in the ISR
    communication_step();
    CHECK(g_endpoint.tx_active() && serial.tx_busy() && dropped == 0u);
    const std::span<const uint8_t> reply{fake::model().tx_src, fake::model().tx_len};
#if DOC_RTU
    peer.receive_adu(reply);
#else
    peer.consume(reply);
#endif
    CHECK(peer.has_packet());
    fake::tx_done();
    communication_step();
    CHECK(!g_endpoint.tx_active() && fake::model().violations.empty());
    return example::finish(DOC_RTU ? "freertos_entry_rtu" : "freertos_entry_cobs");
}
#endif
