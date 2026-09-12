<!-- Author: shpegun60; SPDX-License-Identifier: MIT -->
# FreeRTOS: UART, protocols, adapters and wake

<!-- toc -->

Contents

- [Choose the task loop](#choose-the-task-loop)
- [What happens in the interrupt and in the task](#what-happens-in-the-interrupt-and-in-the-task)
- [Complete communication task](#complete-communication-task)
  - [Includes and types](#includes-and-types)
  - [Start from the owning task](#start-from-the-owning-task)
  - [Receive, build, preserve Busy, send](#receive-build-preserve-busy-send)
  - [Create a static task](#create-a-static-task)
- [Wake API: every public operation](#wake-api-every-public-operation)
- [Raw UART without a protocol adapter](#raw-uart-without-a-protocol-adapter)
- [COBS with wake but without UartAdapter](#cobs-with-wake-but-without-uartadapter)
- [IRQ priorities, ownership and task shutdown](#irq-priorities-ownership-and-task-shutdown)
- [Verify and understand the boundary](#verify-and-understand-the-boundary)

<!-- /toc -->

[Documentation](README.md) · [Почни звідси](START_HERE_UK.md) · [Examples](EXAMPLES.md) · [Qt](QT.md)

This guide assumes the application already has a configured UART handle and
a working FreeRTOS port. It explains how to use the libraries, not which Cube
checkboxes to select. Choose one of the complete combinations below.

## Choose the task loop

| Composition | Before the loop | Task body | Who owns protocol timing? |
|---|---|---|---|
| COBS + UART adapter | attach wake, init UART, bind adapter | `wait(adapter); adapter.proceed();` then pop packets | no COBS stale timer |
| Framed RTU + UART adapter | same | exactly the same body | RTU adapter |
| Raw UART, no protocol | attach wake, set RX/gap handlers, init UART | `wait(50u); serial.proceed(HAL_GetTick());` | application, if needed |
| COBS manually bound to UART | raw UART setup plus Endpoint sender/busy/RX/gap | wait, UART proceed, Endpoint poll, pop | no COBS stale timer |
| RTU without an adapter | application must provide candidate boundaries or a framed stream recovery policy | explicit UART/parser/poll service | application; see [manual integration](INTEGRATION.md#without-an-adapter) |
| Busy-loop/bare metal | no wake object required | adapter proceed, or raw UART proceed + Endpoint poll | same as above |

`FreeRtosWake` works with UART regardless of the protocol. It is not a COBS
or RTU framer. MBAP/TCP can be serviced by its network task independently;
there is no public TCP-specific FreeRTOS/UART adapter to imply otherwise.

## What happens in the interrupt and in the task

```text
USART/DMA ISR: update driver state -> queue RX/gap or finish TX -> WakeHandler
                                                                   |
                                                                   v
                                                    FreeRTOS notification
                                                                   |
communication task: wait returns -> adapter.proceed -> pop_packet -> application
```

The interrupt does not run Endpoint parsing, allocate packet storage or call
your packet handler. RX and ordered gap callbacks execute inside UART
`proceed()` in task context. The UART Tx/Error/Wake handlers can run in ISR
context: keep them ISR-safe, bounded and nonblocking; do not call a protocol
endpoint from them. The driver still performs its required DMA/cache work.
Moving parsing to a task does not mean every ISR is empty.

## Complete communication task

The checked source is [freertos_entry.cpp](examples/freertos_entry.cpp).
The same file builds a COBS echo task or a small RTU request handler. The
host-only `DOC_HOST` section supplies test scaffolding; leave `DOC_HOST`
undefined in firmware. Define `DOC_RTU=1` for RTU, otherwise it uses COBS.

### Includes and types

These declarations are a firmware integration fragment, with all choices
shown. `UART_ENGINE_IMPLEMENT` must appear in exactly one firmware TU; omit
it here if another TU already supplies the driver's HAL callbacks.

```cpp
#define UART_ENGINE_IMPLEMENT
#include "uart/Uart.h"
#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/cobs/UartAdapter.h"
#include "adapters/rtu/UartAdapter.h"
#include "adapters/freertos/FreeRtosWake.h"
#include "FreeRTOS.h"
#include "task.h"

extern UART_HandleTypeDef huart3;
using Serial = Uart<256, 4>;
using Link = cobs::Endpoint<wire::Pool<8, 2>>;
using Adapter = cobs::UartAdapter<Serial, Link>;

static Serial serial;
static Link g_endpoint;
static Adapter adapter{serial, g_endpoint};
static uart::FreeRtosWake wake;
static Link::Message pending;
static unsigned dropped = 0;
// Also define DOC_RTU=0 for the COBS branch of communication_step below.
```

For an RTU server replace only Link/Adapter; keep the UART, wake and service
structure. RX requests and TX responses have different layouts:

```cpp
namespace framing = modbus::rtu::framing;
using Link = modbus::rtu::Endpoint<wire::Pool<8, 2>, modbus::rtu::Format<>,
    framing::Standard<framing::Direction::Request>>;
using Adapter = modbus::rtu::UartAdapter<Serial, Link>;
```

For an RTU client use `Direction::Response`. The core then assembles
responses, but your task still owns scheduling, matching and request timeouts;
it is not the Qt `RtuClient` transplanted to FreeRTOS.

### Start from the owning task

<!-- example: examples/freertos_entry.cpp#freertos-start -->
```cpp
static bool communication_start()
{
    // Executed BY the task, not by its creator after it can already run.
    return wake.attach(serial, xTaskGetCurrentTaskHandle()) &&
           serial.init(&huart3) && adapter.bind();
}
```
<!-- /example -->

Attach using the task's own current handle before UART events can arrive.
Do not create a runnable task and then race it by attaching the callback from
the creator. Constructing a static wake object before the kernel starts is
fine; attaching a null task is refused.

### Receive, build, preserve Busy, send

This is the actual checked loop step, including the demo's drop decisions.
It keeps at most one pending reply and does not pop another Packet while that
reply is waiting. Already queued RX packets still consume pool slots.

<!-- example: examples/freertos_entry.cpp#freertos-service -->
```cpp
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
```
<!-- /example -->

For COBS it echoes the complete payload. For RTU it implements only unit 1,
FC03, register 0/count 1 and replies with value 100. Other requests are
explicitly counted as dropped; this is not advertised as a complete register
server. Extend application validation, exception responses and broadcast
handling deliberately. [RTU client/server example](examples/rtu_framing.cpp)
also shows exception construction and an owned private length prefix.

`Busy` keeps `pending`. This demo drops and counts `Failed`, `Unbound` and
`Invalid`; it does not replay potentially partially transmitted commands.
Choose an application queue/retry policy if losing a request is unacceptable.
Queueing by itself is not an acknowledgement or duplicate-execution guard.

### Create a static task

<!-- example: examples/freertos_entry.cpp#freertos-task -->
```cpp
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
```
<!-- /example -->

Call `start_communication_task()` once after platform initialization and
check its returned handle. Start the scheduler in the application's existing
startup path. The example intentionally has static lifetime, not task deletion
and restart logic. `768` is an example stack element count, not a measured
minimum for your handler. Measure stack headroom under your real load.
[FreeRTOS xTaskCreateStatic reference](https://www.freertos.org/Documentation/02-Kernel/04-API-references/01-Task-creation/02-xTaskCreateStatic).

## Wake API: every public operation

| Operation | Meaning / obligation |
|---|---|
| `uart::FreeRtosWake wake;` | contains a task handle, initially null; no timer or packet allocation |
| `wake.attach(serial, task)` | rejects null; installs the driver's one WakeHandler, borrowing `wake` |
| `wake.task()` | inspect the currently attached handle |
| `wake.notify()` | ISR-side notification/yield hook; normally called only by the installed driver callback |
| `FreeRtosWake::wait(adapter)` | caller is the attached task; wait for notification or the shorter of adapter deadline and 50 ms fallback |
| `FreeRtosWake::wait(adapter, 20u)` | same, with a custom millisecond fallback |
| `FreeRtosWake::wait(50u)` | raw-duration form for integrations without an adapter |
| `serial.setWakeHandler({})` | remove the borrowed callback before wake/task lifetime ends; there is no separate `wake.detach()` |

`wait()` returns the notification count taken (zero for a timeout), not the
number of packets. Always service the driver afterward, even when it returns
zero. Multiple IRQ events can be serviced in one task iteration.

The fallback is a periodic service bound: it allows driver health/recovery
and application work to progress even if no new IRQ arrives. It is not a
receive timeout for COBS and not a complete RTU transaction timeout. If your
application has a shorter deadline, use a correspondingly bounded fallback.

No user `std::min`, clock read or `deadline_in_ms()` call is needed with the
adapter overload. Explicit `proceed(now_ms)` and `deadline_in_ms(now_ms)` are
advanced custom-scheduler/test APIs; never mix their synthetic clock with
automatic HAL-time calls.

## Raw UART without a protocol adapter

The complete [uart_wake.cpp](examples/uart_wake.cpp) copies RX bytes in the
thread-context handler, sends caller-owned TX bytes and checks their lifetime.
There are no Messages, Packets, CRC or implicit packet boundaries.

```cpp
serial.setRxHandler(Serial::RxHandler{
    [](std::span<const uint8_t> bytes) noexcept {
        // Consume now, or copy to your own bounded queue before returning.
    }});
serial.setRxGapHandler(Serial::GapHandler{
    []() noexcept {
        // Some stream bytes were lost here; reset your own parser if needed.
    }});
// In this task: check wake.attach(serial, xTaskGetCurrentTaskHandle()),
// then check serial.init(&huart3).
```

<!-- example: examples/uart_wake.cpp#raw-uart-wake -->
```cpp
static void task_step()
{
    (void)uart::FreeRtosWake::wait(50u); // no adapter: select a fallback directly
    serial.proceed(HAL_GetTick());      // raw driver takes an explicit tick
}
```
<!-- /example -->

The receive span is valid only during its callback. A successful
`serial.send(bytes)` borrows immutable TX bytes until `serial.tx_busy()` is
false; a local array must not disappear while DMA can still read it. The
driver has no hidden TX queue. After a loss, GapHandler marks a stream hole;
it is not interchangeable with the raw HAL error mask from ErrorHandler.

## COBS with wake but without UartAdapter

The checked [cobs_direct.cpp](examples/cobs_direct.cpp) is built twice: plain
polling and with `DOC_WAKE=1`. The manual wiring is complete here as a fragment
to place beside your `serial`, `link` and `wake` objects:

```cpp
serial.setRxHandler(Serial::RxHandler{
    [](std::span<const uint8_t> bytes) noexcept { link.consume(bytes); }});
serial.setRxGapHandler(Serial::GapHandler{
    []() noexcept { link.notify_gap(); }});

const bool bound = link.bind(
    Link::Sender{tiny::bind<&Serial::send>(serial)},
    Link::BusyQuery{tiny::bind<&Serial::tx_busy>(serial)});
// Check bound, attach wake in this task, then check serial.init(&huart3).

for (;;) {
    (void)uart::FreeRtosWake::wait(50u);
    const uint32_t now = HAL_GetTick();
    serial.proceed(now);
    link.poll(now);
    while (auto packet = link.pop_packet()) {
        // Read/retain Packet here; do not send the handle across tasks.
    }
}
```

COBS requires no incomplete-frame timer here. `poll()` reclaims TX; it is not
the call that reads UART or runs the COBS RX parser. That parsing happens in
the RX callback triggered by `serial.proceed()`.

## IRQ priorities, ownership and task shutdown

- Enable FreeRTOS task notifications and reserve notification index 0 for
  this UART wake. Calling `wait()` from another task consumes that other task's
  notifications and is wrong even if it compiles.
- USART/DMA IRQs calling FromISR APIs must use a FreeRTOS-permitted interrupt
  priority. For this Cortex-M configuration, the unshifted CMSIS priority
  number must be at least `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`.
  Use the actual port's priority convention; this is not an IRQ setup tutorial.
- UART, Endpoint, Message and non-atomic Packet copies/releases belong to one
  serialized execution domain. To communicate with other tasks, copy values or
  use an explicitly designed ownership-transfer/synchronization layer.
- Keep both the UART's own RX memory and any directly borrowed TX memory
  DMA-accessible. Pool/Heap chooses allocation, not linker placement. Keep
  cache maintenance enabled unless the entire relevant region is coherently
  non-cacheable by design.
- Do not delete a task while the UART callback still references its handle.
  Quiesce transmission, preserve/prove completion or abort, service ownership,
  unbind the adapter, remove `serial.setWakeHandler({})`, then release handles.
  The static example avoids dynamic teardown; it is not a hot-restart recipe.
- Finite wait conversion never accidentally requests `portMAX_DELAY`; on a
  coarse RTOS tick a sub-tick deadline can require nonblocking service. Choose
  tick resolution appropriate to your timing requirements.

For another scheduler, install an ISR-safe `Serial::WakeHandler` that signals
your own primitive, then service the raw driver or adapter in its owner loop.
Do not call `FreeRtosWake::notify()` from ordinary task code as a generic signal:
it uses the kernel's FromISR API. Application commands can use a separate
RTOS queue/notification path with the matching task-context API.

## Verify and understand the boundary

```sh
sh doc/examples/build.sh
sh src/adapters/tests/run.sh
sh src/adapters/tests/check_wake_codegen.sh
# Optional real H7RS HAL/FreeRTOS task-entry compile, no flashing:
sh doc/examples/check_freertos_arm.sh
```

The cookbook covers 16 host configurations, including raw wake, manual COBS
wake and both task-entry variants. Those use a fake HAL/notification recorder,
not a real scheduler. Compile the entry TU with the real kernel/HAL headers
for your port; static task creation requires `configSUPPORT_STATIC_ALLOCATION`.
The [hardware checkpoint](HARDWARE_EXTENSIONS_2026-09-12.md) separately records
real FreeRTOS on NUCLEO-H7S3L8. This documentation update does not reflash it.
