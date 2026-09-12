/* Author: shpegun60; SPDX-License-Identifier: MIT */
// Manual UART + framed RTU client. No UartAdapter and no inferred IDLE boundary.
// One fixed FC03 request, one application-owned whole-request time budget.
// DOC_HOST supplies only the executable fake-HAL checks at the end.
// example-begin: manual-rtu-types
#define UART_ENGINE_IMPLEMENT // exactly one TU in the firmware
#include "uart/Uart.h"
#include "modbus/rtu/Rtu.h"
#include <algorithm>

#ifndef DOC_WAKE
#define DOC_WAKE 0
#endif
#if DOC_WAKE
#include "adapters/freertos/FreeRtosWake.h"
#endif

namespace manual_rtu {
namespace framing = modbus::rtu::framing;
using Serial = Uart<256, 4>;
using Link = modbus::rtu::Endpoint<wire::Pool<4, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Response>>;

// Place the UART AND its borrowed Pool TX buffers in DMA-accessible RAM.
static Serial serial;
static Link endpoint;
static Link::Message pending;
#if DOC_WAKE
static uart::FreeRtosWake wake;
#endif
enum class Result { Idle, Pending, Waiting, Completed, Timeout, Gap, Failed, ResponseError };
static Result result = Result::Idle;
static bool initialized = false;
static uint16_t value = 0;
static uint32_t started_ms = 0;
constexpr uint32_t request_budget_ms = 1000u; // Busy + TX + response, not RTU t1.5/t3.5

bool active() noexcept { return result == Result::Pending || result == Result::Waiting; }
// example-end: manual-rtu-types

// example-begin: manual-rtu-start
bool start(UART_HandleTypeDef& handle) noexcept
{
    if (!serial.init(&handle) || !endpoint.bind(
            Link::Sender{tiny::bind<&Serial::send>(serial)},
            Link::BusyQuery{tiny::bind<&Serial::tx_busy>(serial)})) { return false; }
    serial.setRxHandler(Serial::RxHandler{[](std::span<const uint8_t> bytes) noexcept {
        if (active()) { endpoint.consume(bytes); } // arbitrary cuts, never receive_adu(chunk)
    }});
    serial.setRxGapHandler(Serial::GapHandler{[]() noexcept {
        endpoint.notify_gap();
        if (active()) { pending = {}; result = Result::Gap; }
    }});
    initialized = true;
    return true;
}

bool begin_read() noexcept
{
    if (!initialized || active() || endpoint.tx_active()) { return false; }
    // Caller establishes a new request boundary; this cannot identify future late responses.
    endpoint.discard_incomplete();
    while (auto packet = endpoint.pop_packet()) {} // drop previously queued unsolicited packets
    pending = endpoint.make_message(0x11u, 0x03u);
    if (!pending || !pending.append_be(uint16_t{0}) || !pending.append_be(uint16_t{1})) {
        pending = {}; result = Result::Failed; return false;
    }
    started_ms = HAL_GetTick();
    result = Result::Pending;
    return true;
}
// example-end: manual-rtu-start

// example-begin: manual-rtu-service
void step() noexcept
{
#if DOC_WAKE
    uint32_t wait_ms = 50u; // periodic UART health service, also when no request is active
    if (active()) {
        const uint32_t elapsed = HAL_GetTick() - started_ms;
        wait_ms = elapsed >= request_budget_ms ? 0u
            : std::min(wait_ms, request_budget_ms - elapsed);
    }
    (void)uart::FreeRtosWake::wait(wait_ms); // called only by the task attached to wake
#endif
    const uint32_t now = HAL_GetTick(); // fresh after waking; one tick for this iteration
    serial.proceed(now);               // RX/gap callbacks execute here, not in ISR
    endpoint.poll(now);                // release only TX memory no longer borrowed

    // Drain already published responses BEFORE applying the application's deadline.
    while (auto packet = endpoint.pop_packet()) {
        if (result != Result::Waiting || packet.address() != 0x11u ||
            (packet.function() != 0x03u && packet.function() != 0x83u)) { continue; }
        std::size_t offset = 0;
        uint8_t count = 0;
        uint16_t received = 0;
        const bool valid = packet.function() == 0x03u &&
            modbus::read_be(packet.data(), offset, count) && count == 2u &&
            modbus::read_be(packet.data(), offset, received) && offset == packet.size();
        if (valid) { value = received; }
        result = valid ? Result::Completed : Result::ResponseError;
        endpoint.discard_incomplete(); // no second response belongs to this one-request demo
    }

    if (active() && static_cast<uint32_t>(now - started_ms) >= request_budget_ms) {
        pending = {};                 // cancels an unsent Message; not an accepted DMA borrow
        endpoint.expire_incomplete();  // explicit transaction abandonment, not a silence guess
        result = Result::Timeout;
    }
    if (result == Result::Pending) {
        const auto sent = endpoint.send(pending);
        if (sent == wire::SendResult::Sent) { result = Result::Waiting; }
        else if (sent != wire::SendResult::Busy) {
            pending = {}; result = Result::Failed; // no automatic physical-error retry
        }
    }
}

bool stop() noexcept
{
    // Keep calling step until the transport has provably released its borrow.
    if (serial.tx_busy() || endpoint.tx_active() || !endpoint.unbind()) { return false; }
    serial.setRxHandler({});
    serial.setRxGapHandler({});
#if DOC_WAKE
    serial.setWakeHandler({});
#endif
    pending = {};
    endpoint.discard_incomplete();
    while (auto packet = endpoint.pop_packet()) {}
    initialized = false;
    result = Result::Idle;
    return true;
}
} // namespace manual_rtu
// example-end: manual-rtu-service

#if DOC_WAKE && !defined(DOC_HOST)
// example-begin: manual-rtu-task
#include "FreeRTOS.h"
#include "task.h"
extern UART_HandleTypeDef huart3;

void communication_task(void*)
{
    if (!manual_rtu::wake.attach(manual_rtu::serial, xTaskGetCurrentTaskHandle()) ||
        !manual_rtu::start(huart3) || !manual_rtu::begin_read()) {
        for (;;) { vTaskSuspend(nullptr); } // application fatal-error policy
    }
    for (;;) {
        manual_rtu::step();
        // Observe manual_rtu::result; Completed supplies manual_rtu::value.
        // This demo sends once. Decide recovery before requesting again.
    }
}
// example-end: manual-rtu-task
#endif

#ifdef DOC_HOST
#include "platform_fake.h"
#include "Example.h"
#include "Test.h"

int main()
{
    using namespace manual_rtu;
    fake::reset();
    configure_huart3(115200u);
#if DOC_WAKE
    fake_freertos::reset();
    CHECK(wake.attach(serial, reinterpret_cast<TaskHandle_t>(0x20001000u)));
#endif
    CHECK(start(huart3));
    const auto reply = modbus_test::make_adu(0x11u, 0x03u, std::array<uint8_t, 3>{2u, 0u, 100u});
    const auto part = [&] {
        fake::rx_bytes(reply.data(), 3u); fake::rx_idle(); step();
        CHECK(endpoint.assembling() && result == Result::Waiting);
    };
    const auto rest = [&] {
        fake::rx_bytes(reply.data() + 3u, reply.size() - 3u); fake::rx_idle();
    };
    const auto request = [&] {
        CHECK(begin_read()); step();
        CHECK(result == Result::Waiting && !pending && endpoint.tx_active());
        fake::tx_done(); step();
        CHECK(!endpoint.tx_active());
    };

    request(); part();
    fake::advance_tick(10u); rest(); step();
    CHECK(result == Result::Completed && value == 100u);

    // At the deadline, a queued complete continuation wins over timeout.
    request(); part();
    fake::advance_tick(request_budget_ms); rest(); step();
    CHECK(result == Result::Completed && !endpoint.assembling());

    // Neither empty consume nor unpublished, nonzero DMA progress extends the fixed budget.
    request(); part();
    fake::rx_bytes(reply.data() + 3u, 1u);
    CHECK(serial.rx_progress() == 1u);
    endpoint.consume({});
    fake::advance_tick(request_budget_ms - 1u); step();
    CHECK(result == Result::Waiting);
    fake::advance_tick(1u); step();
    CHECK(result == Result::Timeout && !endpoint.assembling());
    CHECK(endpoint.framing_stats().stale_frames == 1u);
    fake::rx_idle(); step(); // late bytes ignored; establish a clean test boundary

    request(); part();
    fake::rx_error(HAL_UART_ERROR_ORE); step();
    CHECK(result == Result::Gap && !endpoint.assembling());

    // An unrelated caller's live TX is a deliberate Busy control, not multiplexing advice.
    const std::array<uint8_t, 1> occupied{0u};
    CHECK(serial.send(occupied));
    CHECK(begin_read()); step();
    CHECK(result == Result::Pending && pending && !endpoint.tx_active());
    fake::tx_done(); step();
    CHECK(result == Result::Waiting && !pending && endpoint.tx_active());
    fake::rx_bytes(reply.data(), reply.size()); fake::rx_idle(); step();
    CHECK(result == Result::Completed && endpoint.tx_active());
    CHECK(!begin_read() && !stop()); // early RX completion is NOT TX memory completion
    fake::tx_done(); step();

    // Busy consumes the same budget; an expired unsent request must not start later.
    CHECK(serial.send(occupied));
    CHECK(begin_read()); step();
    fake::model().fail_abort_transmit = 100;
    fake::advance_tick(request_budget_ms); step();
    CHECK(result == Result::Timeout && !pending && serial.tx_busy());
    CHECK(!stop());
    fake::model().fail_abort_transmit = 0;
    fake::tx_done(); step();

    // Application timeout cannot release an accepted DMA borrow, even after failed abort.
    CHECK(begin_read()); step();
    fake::model().fail_abort_transmit = 100;
    fake::advance_tick(request_budget_ms); step();
    CHECK(result == Result::Timeout && endpoint.tx_active());
    CHECK(!begin_read() && !stop());
    fake::model().fail_abort_transmit = 0;
    fake::tx_done(); step();
    CHECK(!endpoint.tx_active());

    fake::model().tick = 0xfffffff0u;
    request(); part();
    fake::advance_tick(request_budget_ms - 1u); step();
    CHECK(result == Result::Waiting);
#if DOC_WAKE
    step(); // next bounded wait sees one millisecond remaining across tick wrap
    CHECK(fake_freertos::model().last_take_timeout == 1u);
#endif
    fake::advance_tick(1u); step();
    CHECK(result == Result::Timeout && !endpoint.assembling());

    request();
    const auto exception = modbus_test::make_adu(0x11u, 0x83u, std::array<uint8_t, 1>{2u});
    fake::rx_bytes(exception.data(), exception.size()); fake::rx_idle(); step();
    CHECK(result == Result::ResponseError);

    CHECK(endpoint.unbind()); // deliberate Unbound control: fail without automatic retry
    CHECK(begin_read()); step();
    CHECK(result == Result::Failed && !pending);
    CHECK(stop());
    CHECK(!begin_read() && fake::model().violations.empty());
    return example::finish("rtu_uart_direct");
}
#endif
