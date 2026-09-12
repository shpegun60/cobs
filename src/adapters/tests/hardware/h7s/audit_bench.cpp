/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 *
 * Live-silicon regressions for the September 2026 paranoid audit. This is a
 * separate firmware, never an injection hook in the production driver.
 * USART3/GPDMA receive real VCP bytes; only the failure being tested is
 * injected (lost RX publication or a disabled peripheral DMA request).
 * All deadlines use the real HAL tick, with I/D caches enabled. See run.py.
 */
#define UART_ENGINE_IMPLEMENT
#include "uart/Uart.h"
#include "modbus/rtu/Rtu.h"
#include "adapters/rtu/UartAdapter.h"
#include "uart/tests/bench/uart_bench.h"
#include "usart.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

extern "C" {
BenchCounter g_bench_usart_irq{};
BenchCounter g_bench_rx_dma_irq{};
BenchCounter g_bench_tx_dma_irq{};
}

namespace {
namespace framing = modbus::rtu::framing;
using framing::Direction;
using framing::Layout;
using Serial = Uart<256, 4>;
constexpr uint32_t baud = 9600u;

template<std::size_t Width, std::endian Order = std::endian::big>
struct Prefix : framing::Standard<Direction::Request> {
    static constexpr Layout layout(Direction, uint8_t function) noexcept
    {
        return function == 0x41u ? Layout::length_prefixed(Width, Order) : Layout::unsupported();
    }
};

// Volatile input forces the guarded factory/receiver path to execute on the
// MCU instead of reducing this negative vector to a constant at compile time.
static volatile std::size_t invalid_offset = std::numeric_limits<std::size_t>::max();
struct RuntimeOffset : framing::Standard<Direction::Request> {
    static Layout layout(Direction, uint8_t) noexcept { return Layout::byte_count_at(invalid_offset); }
};

using Link = modbus::rtu::Endpoint<wire::Pool<4, 1>, modbus::rtu::Format<crc::Crc16Bitwise, 1020>, Prefix<2>>;
static Serial serial;
static Link link;
static modbus::rtu::UartAdapter adapter{serial, link};

struct Checks {
    uint32_t count = 0, failed = 0, first = 0;
    void test(bool ok) noexcept
    {
        ++count;
        if (!ok) {
            ++failed;
            if (first == 0u) { first = count; }
        }
    }
};

struct Report {
    char kind = 'R', command = 0;
    uint32_t stage = 0;
    Checks checks{};
    uint32_t a = 0, b = 0, c = 0, d = 0;
};
static std::array<Report, 8> reports{};
static std::size_t put = 0, get = 0;
static std::array<char, 160> tx_text{}; // unchanged while DMA borrows it

void report(Report value) noexcept
{
    if (put - get >= reports.size()) { Error_Handler(); }
    reports[put++ % reports.size()] = value;
}

void pump_report() noexcept
{
    if (put == get || serial.tx_busy()) { return; }
    const auto& r = reports[get % reports.size()];
    std::size_t size = 0u;
    for (const char ch : "AUDIT ") { if (ch != 0) { tx_text[size++] = ch; } }
    tx_text[size++] = r.kind;
    tx_text[size++] = ' ';
    tx_text[size++] = r.command;
    for (uint32_t value : {r.stage, r.checks.count, r.checks.failed, r.checks.first, r.a, r.b, r.c, r.d}) {
        tx_text[size++] = ' ';
        std::array<char, 10> digits{};
        std::size_t count = 0u;
        do {
            digits[count++] = static_cast<char>('0' + value % 10u);
            value /= 10u;
        } while (value != 0u);
        while (count != 0u) { tx_text[size++] = digits[--count]; }
    }
    tx_text[size++] = '\n'; // bounded by 9 + 8 * 11 + 1 < 160
    if (serial.send({reinterpret_cast<const uint8_t*>(tx_text.data()), size})) { ++get; }
}

static char command = 0, pending = 0;
static uint32_t started = 0, prefix_tick = 0, gaps = 0, stage = 0, received = 0;
static uint32_t base_gaps = 0, base_restarts = 0, base_errors = 0, base_stale = 0;
static uint32_t extensions = 0, last_progress = 0, first_deadline = 0;
static bool zero_busy_seen = false, frame_mode = false, finishing = false;
static Checks checks;
static Report final_report;
static std::array<uint8_t, 64> fault_tx{};
static volatile uint32_t tx_verdicts = 0u;
static volatile bool tx_verdict_ok = false;
static uint32_t base_tx_verdicts = 0u, rx_dma_address = 0u;
static bool fault_borrow_retained = false, gap_before_rx_stop = false;

void idle_enable(bool enabled) noexcept
{
    if (enabled) {
        __HAL_UART_CLEAR_IDLEFLAG(&huart3);
        __HAL_UART_ENABLE_IT(&huart3, UART_IT_IDLE);
    } else {
        __HAL_UART_DISABLE_IT(&huart3, UART_IT_IDLE);
    }
}

void ready(uint32_t step) noexcept { report({'R', command, step}); }

void end(uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) noexcept
{
    final_report = {'T', command, 0u, checks, a, b, c, d};
    finishing = true;
    frame_mode = false;
}

void clean_and_report() noexcept
{
    if (!finishing || serial.tx_busy() || get != put) { return; }
    // Discard the deliberately unpublished byte only AFTER observations.
    // Rearm via the public API, not by forging the driver's private state.
    idle_enable(true);
    HAL_NVIC_ClearPendingIRQ(GPDMA1_Channel11_IRQn);
    HAL_NVIC_EnableIRQ(GPDMA1_Channel11_IRQn);
    if (!serial.setBaudRate(baud)) { Error_Handler(); }
    adapter.proceed(HAL_GetTick());
    link.discard_incomplete();
    while (auto packet = link.pop_packet()) { packet.reset(); }
    final_report.checks.test(link.storage().rx_available() == 4u);
    report(final_report);
    command = 0;
    finishing = false;
}

template<class Base>
struct CountingCrc {
    using value_type = typename Base::value_type;
    static constexpr std::size_t wire_size = Base::wire_size;
    unsigned* calls;
    value_type calculate(std::span<const uint8_t> bytes) noexcept
    {
        ++*calls;
        return Base{}.calculate(bytes);
    }
    static void store(uint8_t* bytes, value_type value) noexcept { Base::store(bytes, value); }
    static value_type load(const uint8_t* bytes) noexcept { return Base::load(bytes); }
};

struct Capture {
    std::array<uint8_t, 512> bytes{};
    std::size_t size = 0;
    unsigned calls = 0;
    bool busy() const noexcept { return false; }
    bool send(std::span<const uint8_t> data) noexcept
    {
        ++calls;
        if (data.size() > bytes.size()) { return false; }
        size = data.size();
        std::copy(data.begin(), data.end(), bytes.begin());
        return true;
    }
};

template<class Crc, std::size_t Width, std::endian Order = std::endian::big>
void prefix_checks() noexcept
{
    using Device = modbus::rtu::Endpoint<wire::Pool<2, 1>,
        modbus::rtu::Format<CountingCrc<Crc>, 510u - Crc::wire_size>, Prefix<Width, Order>>;
    unsigned crc_calls = 0;
    Device device{CountingCrc<Crc>{&crc_calls}};
    Capture wire;
    checks.test(device.bind(typename Device::Sender{tiny::bind<&Capture::send>(wire)},
        typename Device::BusyQuery{tiny::bind<&Capture::busy>(wire)}));
    std::array<uint8_t, 512> body{};
    body.fill(0x5Au);
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{254},
            std::size_t{255}, std::size_t{256}, std::size_t{300}, Device::max_send_size - Width}) {
        auto message = device.make_message(0x11u, 0x41u, size + Width);
        checks.test(message && message.append_bytes({body.data(), size}));
        const unsigned crc_before = crc_calls, sends_before = wire.calls;
        const auto result = device.send(message);
        if (Width == 1u && size > UINT8_MAX) {
            checks.test(result == modbus::SendResult::Invalid);
            checks.test(message && message.size() == size + Width && message.append_bytes({}));
            checks.test(crc_calls == crc_before && wire.calls == sends_before && !device.tx_active());
        } else {
            checks.test(result == modbus::SendResult::Sent && !message);
            const std::span<const uint8_t> bytes{wire.bytes.data(), wire.size};
            checks.test(wire.size == 2u + Width + size + Crc::wire_size);
            checks.test(Prefix<Width, Order>::layout(Direction::Request, 0x41).matches(bytes.subspan(2u, Width + size)));
            device.consume(bytes.first(2u + Width - 1u));
            checks.test(device.assembling() && !device.has_packet());
            device.consume(bytes.subspan(2u + Width - 1u));
            auto packet = device.pop_packet();
            checks.test(packet && packet.size() == Width + size &&
                std::equal(packet.data().begin() + Width, packet.data().end(), body.begin(), body.begin() + size));
            packet.reset();
            checks.test(!device.assembling() && !device.has_packet() && device.stats().rx.crc_errors == 0u);
            device.poll(0u);
        }
    }
    checks.test(device.storage().rx_available() == 2u && device.storage().tx_available() == 1u);
}

void local_checks() noexcept
{
    prefix_checks<crc::Crc16Bitwise, 1>();
    prefix_checks<crc::Crc16Table, 1>();
    prefix_checks<crc::NoCrc, 1>();
    prefix_checks<crc::Crc16Bitwise, 2>();
    prefix_checks<crc::Crc16Bitwise, 2, std::endian::little>();
    const std::size_t maximum = std::numeric_limits<std::size_t>::max();
    for (std::size_t width = 1u; width <= 2u; ++width) {
        for (std::size_t back = 0u; back < 16u; ++back) {
            checks.test(Layout::byte_count_at(maximum - back, width).kind == Layout::Kind::Unsupported);
        }
        std::array<uint8_t, 16> bytes{};
        bytes.fill(0xA5u);
        const auto original = bytes;
        checks.test(!Layout::length_prefixed(width).store_count(bytes.data(), maximum) && bytes == original);
    }
    modbus::rtu::Endpoint<wire::Pool<2, 1>, modbus::rtu::Format<>, RuntimeOffset> invalid;
    const std::array<uint8_t, 4> candidate{0x11u, 0x41u, 0u, 0u};
    invalid.consume(std::span{candidate}.first(2u));
    checks.test(!invalid.assembling() && !invalid.has_packet() &&
        invalid.framing_stats().unsupported_function == 1u && invalid.storage().rx_available() == 2u);
    invalid.receive_adu(candidate);
    checks.test(!invalid.has_packet() && invalid.framing_stats().unsupported_function == 2u &&
        invalid.storage().rx_available() == 2u);
    end();
}

void start(char next, uint32_t now) noexcept
{
    command = next;
    checks = {};
    started = now;
    stage = received = extensions = last_progress = first_deadline = 0u;
    zero_busy_seen = false;
    base_gaps = gaps;
    base_restarts = serial.stats().restarts;
    base_errors = serial.stats().rx_errors;
    base_stale = link.framing_stats().stale_frames;
    base_tx_verdicts = tx_verdicts;
    fault_borrow_retained = gap_before_rx_stop = false;
    frame_mode = next == 'P' || next == 'A' || next == 'E' || next == 'F';
    if (next == 'L') { local_checks(); }
    else if (next == 'H') {
        checks.test(huart3.RxState == HAL_UART_STATE_BUSY_RX && serial.rx_progress() == 0u);
        checks.test((huart3.Instance->CR3 & USART_CR3_DMAR) != 0u);
        checks.test(link.storage().rx_available() == 4u);
        checks.test((SCB->CCR & (SCB_CCR_IC_Msk | SCB_CCR_DC_Msk)) == (SCB_CCR_IC_Msk | SCB_CCR_DC_Msk));
        checks.test((huart3.Instance->CR1 & USART_CR1_FIFOEN) != 0u);
        end(SystemCoreClock, baud, __HAL_DMA_GET_COUNTER(huart3.hdmarx), SCB->CCR);
    } else if (next == 'I') {
        // Receive a real prefix without allowing the normal IDLE callback to
        // end DMA first. The loop then injects the UART-READY/DMA-live state
        // left by HAL's ignored IDLE abort failure, not a forged DMA buffer.
        idle_enable(false);
        ready(1u);
    } else if (next == 'J' || next == 'K') {
        fault_tx.fill('\n'); // any already-shifted bytes are harmless blank lines
        checks.test(serial.send(fault_tx));
        checks.test(serial.tx_busy() && __HAL_DMA_GET_COUNTER(huart3.hdmatx) != 0u);
        DMA_HandleTypeDef* const failing = next == 'J' ? huart3.hdmarx : huart3.hdmatx;
        // Stop ONLY the failing channel. Invoke ST's installed UART_DMAError
        // callback while the sibling channel is really live on this board.
        checks.test(HAL_DMA_Abort(failing) == HAL_OK);
        rx_dma_address = huart3.hdmarx->Instance->CDAR;
        {
            uart::detail::IrqGuard guard;
            failing->ErrorCode = HAL_DMA_ERROR_DTE;
            failing->XferErrorCallback(failing);
            fault_borrow_retained = next == 'J' ? serial.tx_busy()
                : huart3.hdmarx->State == HAL_DMA_STATE_BUSY;
            checks.test(fault_borrow_retained);
            checks.test(next == 'J' ? tx_verdicts == base_tx_verdicts
                : tx_verdicts == base_tx_verdicts + 1u && !tx_verdict_ok);
        }
        stage = 1u;
    } else if (next == 'D') {
        checks.test(huart3.RxState == HAL_UART_STATE_BUSY_RX);
        CLEAR_BIT(huart3.Instance->CR3, USART_CR3_DMAR);
        checks.test(huart3.RxState == HAL_UART_STATE_BUSY_RX && (huart3.Instance->CR3 & USART_CR3_DMAR) == 0u);
    } else if (next == 'Z') {
        idle_enable(false);
        HAL_NVIC_DisableIRQ(GPDMA1_Channel11_IRQn);
        ready(1u);
    } else if (frame_mode) { ready(1u); }
    else if (next != 'Q') { checks.test(false); end(); }
}

void on_rx(std::span<const uint8_t> bytes) noexcept
{
    if (!frame_mode) {
        if (command == 0 && pending == 0 && bytes.size() == 1u) { pending = static_cast<char>(bytes[0]); }
        else if (command != 0) { received += static_cast<uint32_t>(bytes.size()); }
        return;
    }
    received += static_cast<uint32_t>(bytes.size());
    adapter.on_rx(bytes);
    if (stage == 0u && received >= 256u) {
        checks.test(bytes.size() == 256u && received == 256u && link.assembling());
        prefix_tick = HAL_GetTick();
        first_deadline = adapter.deadline_in_ms(prefix_tick);
        checks.test(first_deadline >= 324u && first_deadline <= 325u);
        stage = 1u;
        if (command == 'E') {
            adapter.on_rx({});
            checks.test(adapter.deadline_in_ms(prefix_tick) == first_deadline);
        } else if (command == 'P' || command == 'A') {
            idle_enable(false);
            ready(2u);
        }
    }
}

void frame_step(uint32_t now, bool due, uint32_t progress) noexcept
{
    if (due && link.assembling() && adapter.deadline_armed() && adapter.deadline_in_ms(now) > 0u) {
        ++extensions;
        last_progress = progress;
        if (command == 'A' && extensions == 1u) { ready(3u); }
        if (command == 'A' && extensions == 2u) { idle_enable(true); ready(4u); }
    }
    if (command == 'P' && link.framing_stats().stale_frames != base_stale) {
        checks.test(extensions == 1u && last_progress == 1u && serial.rx_progress() == 1u);
        checks.test(link.framing_stats().stale_frames == base_stale + 1u && !link.assembling() && !adapter.deadline_armed());
        checks.test(link.storage().rx_available() == 4u && !link.has_packet());
        checks.test(serial.stats().restarts == base_restarts && gaps == base_gaps && serial.stats().rx_errors == base_errors);
        end(extensions, last_progress, now - prefix_tick, received);
        return;
    }
    if (command == 'E' && stage == 1u && now - prefix_tick >= 100u) {
        checks.test(link.assembling() && link.framing_stats().stale_frames == base_stale);
        stage = 2u;
        ready(4u);
    }
    if (auto packet = link.pop_packet()) {
        checks.test(packet.adu().size() == 306u && packet.data().size() == 302u);
        bool correct = packet.data().size() == 302u;
        for (std::size_t i = 2u; i < packet.data().size(); ++i) {
            correct = correct && packet.data()[i] == static_cast<uint8_t>((i - 2u) * 37u + 0x5Au);
        }
        checks.test(correct && packet.address() == 0x11u && packet.function() == 0x41u);
        checks.test(link.framing_stats().stale_frames == base_stale && link.stats().rx.crc_errors == 0u);
        checks.test(!link.assembling() && !adapter.deadline_armed());
        if (command == 'A') { checks.test(extensions == 2u && last_progress == 2u); }
        packet.reset();
        checks.test(link.storage().rx_available() == 4u);
        end(extensions, last_progress, now - prefix_tick, received);
    }
}
} // namespace

extern "C" void bench_init(void)
{
    SCB_EnableICache();
    SCB_EnableDCache();
    huart3.Init.BaudRate = baud;
    if (HAL_UART_Init(&huart3) != HAL_OK ||
        HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK ||
        HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK ||
        HAL_UARTEx_EnableFifoMode(&huart3) != HAL_OK || !serial.init(&huart3) || !adapter.bind()) { Error_Handler(); }
    serial.setRxHandler([](std::span<const uint8_t> bytes) noexcept { on_rx(bytes); });
    serial.setRxGapHandler([]() noexcept {
        if (command == 'K' && stage == 1u &&
            huart3.hdmarx->State == HAL_DMA_STATE_BUSY &&
            huart3.hdmarx->Instance->CDAR == rx_dma_address) { gap_before_rx_stop = true; }
        ++gaps;
        adapter.on_gap();
    });
    serial.setTxHandler([](bool ok) noexcept {
        tx_verdict_ok = ok;
        tx_verdicts = tx_verdicts + 1u;
    });
}

extern "C" void bench_loop(void)
{
    const uint32_t now = HAL_GetTick();
    const bool due = frame_mode && adapter.deadline_armed() && adapter.deadline_in_ms(now) == 0u;
    const uint32_t progress = serial.rx_progress();
    if (command == 'I' && stage == 0u && progress == 8u) {
        checks.test(huart3.hdmarx->State == HAL_DMA_STATE_BUSY);
        {
            uart::detail::IrqGuard guard;
            CLEAR_BIT(huart3.Instance->CR3, USART_CR3_DMAR);
            huart3.RxState = HAL_UART_STATE_READY;
            huart3.RxEventType = HAL_UART_RXEVENT_IDLE;
            uart::detail::Registry::onRxEvent(&huart3, 8u);
            checks.test(huart3.hdmarx->State == HAL_DMA_STATE_BUSY && received == 0u);
        }
        stage = 1u;
    }
    if (command == 'Z' && __HAL_DMA_GET_COUNTER(huart3.hdmarx) == 0u && huart3.RxState == HAL_UART_STATE_BUSY_RX) {
        zero_busy_seen = true;
    }
    adapter.proceed(now);
    if (pending != 0 && command == 0 && !finishing) {
        const char next = pending;
        pending = 0;
        start(next, now);
    }
    if (command != 0 && !finishing) {
        if ((command == 'I' || command == 'J' || command == 'K') && stage == 1u &&
            gaps > base_gaps && huart3.RxState == HAL_UART_STATE_BUSY_RX && !serial.tx_busy()) {
            checks.test(gaps == base_gaps + 1u && received == 0u);
            checks.test(serial.stats().restarts == base_restarts + 1u);
            checks.test(__HAL_DMA_GET_COUNTER(huart3.hdmarx) == 256u &&
                (huart3.Instance->CR3 & USART_CR3_DMAR) != 0u);
            if (command != 'I') {
                checks.test(tx_verdicts == base_tx_verdicts + 1u && !tx_verdict_ok);
                checks.test(!gap_before_rx_stop && huart3.hdmatx->State == HAL_DMA_STATE_READY);
            }
            end(serial.stats().restarts - base_restarts, gaps - base_gaps, received,
                command == 'I' ? 0u : tx_verdicts - base_tx_verdicts);
        } else if (command == 'Q' && now - started >= 800u) {
            checks.test(serial.stats().restarts == base_restarts && serial.stats().rx_errors == base_errors);
            checks.test(gaps == base_gaps && serial.rx_progress() == 0u && huart3.RxState == HAL_UART_STATE_BUSY_RX);
            end(now - started);
        } else if ((command == 'D' || command == 'Z') && serial.stats().restarts > base_restarts && gaps > base_gaps) {
            checks.test(serial.stats().restarts == base_restarts + 1u && gaps == base_gaps + 1u);
            checks.test(huart3.RxState == HAL_UART_STATE_BUSY_RX && __HAL_DMA_GET_COUNTER(huart3.hdmarx) == 256u);
            checks.test((huart3.Instance->CR3 & USART_CR3_DMAR) != 0u && received == 0u);
            if (command == 'Z') { checks.test(zero_busy_seen); }
            end(now - started, serial.stats().restarts - base_restarts, gaps - base_gaps, zero_busy_seen ? 1u : 0u);
        } else if (frame_mode) { frame_step(now, due, progress); }
        if (!finishing && now - started > 4000u) { checks.test(false); end(stage, received, extensions, progress); }
    }
    clean_and_report();
    pump_report();
}
