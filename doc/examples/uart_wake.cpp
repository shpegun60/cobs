/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Raw UART and sleeping task: no protocol, CRC, framer or protocol adapter.
#define UART_ENGINE_IMPLEMENT
#include "uart/Uart.h"
#include "adapters/freertos/FreeRtosWake.h"
#include "platform_fake.h"
#include "Example.h"

using Serial = Uart<128, 8>;
static Serial serial;
static uart::FreeRtosWake wake;
static std::array<uint8_t, 16> received{};
static std::size_t received_size = 0;
static unsigned gaps = 0;

static void receive(std::span<const uint8_t> bytes) noexcept
{
    // THREAD context. The RX span expires on return: copy what must survive.
    if (bytes.size() > received.size() - received_size) { ++gaps; return; }
    std::copy(bytes.begin(), bytes.end(), received.begin() + static_cast<std::ptrdiff_t>(received_size));
    received_size += bytes.size();
}

// example-begin: raw-uart-wake
static void task_step()
{
    (void)uart::FreeRtosWake::wait(50u); // no adapter: select a fallback directly
    serial.proceed(HAL_GetTick());      // raw driver takes an explicit tick
}
// example-end: raw-uart-wake

int main()
{
    fake::reset();
    fake_freertos::reset();
    configure_huart3(115200u);
    serial.setRxHandler(Serial::RxHandler{receive});
    serial.setRxGapHandler(Serial::GapHandler{[]() noexcept { ++gaps; }});
    CHECK(wake.attach(serial, reinterpret_cast<TaskHandle_t>(0x20001000u)));
    CHECK(serial.init(&huart3));
    const std::array<uint8_t, 3> input{1, 2, 3};
    fake::rx_bytes(input.data(), input.size());
    fake::rx_idle();
    CHECK(received_size == 0u); // ISR woke the task; it did not process input
    task_step();
    CHECK(received_size == input.size() && std::equal(input.begin(), input.end(), received.begin()));
    CHECK(serial.send(input)); // input stays alive/immutable until TX idle
    CHECK(serial.tx_busy());
    fake::tx_done();
    task_step();
    CHECK(!serial.tx_busy() && gaps == 0u && fake::model().violations.empty());
    serial.setWakeHandler({}); // before the wake object or its task can disappear
    return example::finish("uart_wake");
}
