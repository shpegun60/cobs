/*
 * End-to-end COBS + UART hardware harness for NUCLEO-H7S3L8.
 *
 * The Cube scaffold supplies USART3, its two GPDMA channels, the instrumented
 * IRQ handlers, and calls bench_init()/bench_loop(). This file owns the real
 * production stack under test:
 *
 *   PC reference codec <-> USART3/VCP <-> Uart<128,8>
 *                      <-> Endpoint<Pool<8,2,Format<1024>>>
 *
 * Ordinary application bodies are echoed exactly. Bodies beginning with the
 * reserved four-byte magic below are harness control packets. All responses
 * are ordinary engine frames as well; there is no raw command side channel.
 */

#define UART_ENGINE_PROBE 1
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"

#include "Cobs.h"
#include "uart_bench.h"
#include "usart.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#ifndef COBS_HW_BAUD
#define COBS_HW_BAUD 115200u
#endif

static_assert(COBS_HW_BAUD > 0u, "COBS hardware baud must be non-zero");

BenchCounter g_bench_usart_irq;
BenchCounter g_bench_rx_dma_irq;
BenchCounter g_bench_tx_dma_irq;

namespace {

constexpr std::size_t kMaxPayload = 1024u;
constexpr std::size_t kRxBlocks = 8u;
constexpr std::size_t kTxBlocks = 2u;
constexpr std::size_t kUartChunkSize = 128u;
constexpr std::size_t kUartChunkCount = 8u;

using Wire = cobs::Format<kMaxPayload>;
using Memory = cobs::Pool<kRxBlocks, kTxBlocks, Wire>;
using Link = cobs::Endpoint<Memory>;
using Serial = Uart<kUartChunkSize, kUartChunkCount>;

static_assert(Link::length_size == 2u);
static_assert(Link::max_receive_size == kMaxPayload);
static_assert(Link::max_send_size == kMaxPayload);

constexpr std::array<uint8_t, 4> kMagic{0xC7u, 0x43u, 0x42u, 0x53u};
constexpr uint32_t kProtocolVersion = 1u;
constexpr uint32_t kMaxActionMs = 5000u;

enum class Command : uint8_t {
	Hello = 1u,
	Stats = 2u,
	ResetMetrics = 3u,
	HoldPackets = 4u,
	StallLoop = 5u,
	BackpressureSelfTest = 6u,
};

enum class PendingAction : uint8_t { None, ResetMetrics, HoldPackets, StallLoop };

Serial s_uart;
Link s_link;

BenchCounter s_cobs_consume;
BenchCounter s_cobs_tx_release;
BenchCounter s_packet_process;

struct AppMetrics final {
	uint32_t echo_frames = 0u;
	uint32_t echo_bytes = 0u;
	uint32_t control_frames = 0u;
	uint32_t response_failures = 0u;
	uint32_t selftest_failures = 0u;
};

AppMetrics s_app;
cobs::Stats s_cobs0;
Serial::Stats s_uart0;
cobs::detail::PoolStats s_rxPool0;
cobs::detail::PoolStats s_txPool0;
uint32_t s_windowStart = 0u;

PendingAction s_pendingAction = PendingAction::None;
uint32_t s_pendingMs = 0u;
uint32_t s_holdUntil = 0u;
uint32_t s_stallUntil = 0u;
bool s_holdActive = false;
bool s_stallActive = false;

class Writer final {
public:
	bool putU8(const uint8_t value) noexcept
	{
		if (m_size == m_bytes.size()) {
			return false;
		}
		m_bytes[m_size++] = value;
		return true;
	}

	bool putU32(const uint32_t value) noexcept
	{
		for (unsigned shift = 0u; shift < 32u; shift += 8u) {
			if (!putU8(static_cast<uint8_t>(value >> shift))) {
				return false;
			}
		}
		return true;
	}

	bool putU64(const uint64_t value) noexcept
	{
		for (unsigned shift = 0u; shift < 64u; shift += 8u) {
			if (!putU8(static_cast<uint8_t>(value >> shift))) {
				return false;
			}
		}
		return true;
	}

	bool putBytes(const std::span<const uint8_t> bytes) noexcept
	{
		if (bytes.size() > m_bytes.size() - m_size) {
			return false;
		}
		if (!bytes.empty()) {
			std::memcpy(m_bytes.data() + m_size, bytes.data(), bytes.size());
		}
		m_size += bytes.size();
		return true;
	}

	[[nodiscard]] std::span<const uint8_t> data() const noexcept
	{
		return {m_bytes.data(), m_size};
	}

private:
	std::array<uint8_t, 384> m_bytes{};
	std::size_t m_size = 0u;
};

[[nodiscard]] uint32_t readU32(
	const std::span<const uint8_t> bytes, const std::size_t offset) noexcept
{
	return static_cast<uint32_t>(bytes[offset]) |
		(static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
		(static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
		(static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

[[nodiscard]] uint32_t delta(
	const uint32_t value, const uint32_t baseline) noexcept
{
	return value - baseline;
}

[[nodiscard]] bool deadlinePending(
	const uint32_t now, const uint32_t deadline) noexcept
{
	return static_cast<int32_t>(now - deadline) < 0;
}

template<class Counter>
void putCounter(Writer& writer, const Counter& counter) noexcept
{
	(void)writer.putU64(counter.total);
	(void)writer.putU32(counter.calls);
	(void)writer.putU32(counter.max);
}

[[nodiscard]] bool beginResponse(
	Writer& writer, const Command command, const uint32_t token) noexcept
{
	return writer.putBytes(kMagic) &&
		writer.putU8(static_cast<uint8_t>(command) | 0x80u) &&
		writer.putU32(token);
}

[[nodiscard]] bool sendBody(const std::span<const uint8_t> body) noexcept
{
	auto message = s_link.make_message(body.size());
	if (!message || (!body.empty() && !message.append_bytes(body)) ||
			s_link.send(message) != cobs::SendResult::Sent) {
		++s_app.response_failures;
		return false;
	}
	return true;
}

[[nodiscard]] bool sendWriter(const Writer& writer) noexcept
{
	return sendBody(writer.data());
}

void resetMetrics() noexcept
{
	// IRQ-owned UART diagnostics and 64-bit DWT totals must share the same
	// cut.  On Cortex-M7 an unguarded uint64_t copy can tear, and resetting the
	// probes after taking an unguarded UART baseline would make the reported
	// window internally inconsistent by one event.
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	s_cobs0 = s_link.stats();
	s_uart0 = s_uart.stats();
	s_rxPool0 = s_link.storage().rx_stats();
	s_txPool0 = s_link.storage().tx_stats();
	s_app = {};
	s_windowStart = HAL_GetTick();

	g_bench_usart_irq = {};
	g_bench_rx_dma_irq = {};
	g_bench_tx_dma_irq = {};
	uart_probe::g_stats = {};
	s_cobs_consume = {};
	s_cobs_tx_release = {};
	s_packet_process = {};
	__set_PRIMASK(primask);
}

[[nodiscard]] bool sendAck(
	const Command command, const uint32_t token,
	const uint32_t status, const uint32_t value) noexcept
{
	Writer writer;
	return beginResponse(writer, command, token) &&
		writer.putU32(status) && writer.putU32(value) && sendWriter(writer);
}

[[nodiscard]] bool sendHello(const uint32_t token) noexcept
{
	Writer writer;
	return beginResponse(writer, Command::Hello, token) &&
		writer.putU32(kProtocolVersion) &&
		writer.putU32(COBS_HW_BAUD) &&
		writer.putU32(SystemCoreClock) &&
		writer.putU32(static_cast<uint32_t>(Link::max_receive_size)) &&
		writer.putU32(static_cast<uint32_t>(Link::max_send_size)) &&
		writer.putU32(static_cast<uint32_t>(Link::length_size)) &&
		writer.putU32(static_cast<uint32_t>(kUartChunkSize)) &&
		writer.putU32(static_cast<uint32_t>(kUartChunkCount)) &&
		writer.putU32(static_cast<uint32_t>(kRxBlocks)) &&
		writer.putU32(static_cast<uint32_t>(kTxBlocks)) &&
		sendWriter(writer);
}

[[nodiscard]] bool sendStats(const uint32_t token) noexcept
{
	// One coherent observation point.  In particular, BenchCounter::total is
	// 64-bit on a 32-bit core and is written by IRQs, so direct live reads are
	// not merely approximate: they can be torn into an impossible value.
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	const cobs::Stats cobsStats = s_link.stats();
	const Serial::Stats uartStats = s_uart.stats();
	const cobs::detail::PoolStats rxPool = s_link.storage().rx_stats();
	const cobs::detail::PoolStats txPool = s_link.storage().tx_stats();
	const AppMetrics app = s_app;
	const uint32_t windowMs = HAL_GetTick() - s_windowStart;
	const BenchCounter usartIrq = g_bench_usart_irq;
	const BenchCounter rxDmaIrq = g_bench_rx_dma_irq;
	const BenchCounter txDmaIrq = g_bench_tx_dma_irq;
	const uart_probe::Stats uartProbe = uart_probe::g_stats;
	const BenchCounter cobsConsume = s_cobs_consume;
	const BenchCounter cobsTxRelease = s_cobs_tx_release;
	const BenchCounter packetProcess = s_packet_process;
	__set_PRIMASK(primask);

	Writer writer;
	if (!beginResponse(writer, Command::Stats, token)) {
		return false;
	}

	// The order is a versioned wire contract mirrored by cobs_hardware.py.
	const std::array<uint32_t, 31> values{
		kProtocolVersion,
		windowMs,
		app.echo_frames,
		app.echo_bytes,
		app.control_frames,
		app.response_failures,
		app.selftest_failures,
		delta(uartStats.rx_overrun, s_uart0.rx_overrun),
		delta(uartStats.rx_errors, s_uart0.rx_errors),
		delta(uartStats.tx_errors, s_uart0.tx_errors),
		delta(uartStats.restarts, s_uart0.restarts),
		delta(cobsStats.rx.frames_delivered, s_cobs0.rx.frames_delivered),
		delta(cobsStats.rx.frames_lost, s_cobs0.rx.frames_lost),
		delta(cobsStats.rx.allocation_failure, s_cobs0.rx.allocation_failure),
		delta(cobsStats.rx.malformed, s_cobs0.rx.malformed),
		delta(cobsStats.rx.oversize, s_cobs0.rx.oversize),
		delta(cobsStats.rx.length_mismatch, s_cobs0.rx.length_mismatch),
		delta(cobsStats.rx.resyncs, s_cobs0.rx.resyncs),
		delta(cobsStats.tx.frames_sent, s_cobs0.tx.frames_sent),
		delta(cobsStats.tx.send_refused_busy, s_cobs0.tx.send_refused_busy),
		delta(cobsStats.tx.send_failed, s_cobs0.tx.send_failed),
		static_cast<uint32_t>(kRxBlocks) - rxPool.in_use,
		rxPool.in_use,
		rxPool.high_water,
		delta(rxPool.exhausted, s_rxPool0.exhausted),
		delta(rxPool.rejected, s_rxPool0.rejected),
		static_cast<uint32_t>(kTxBlocks) - txPool.in_use,
		txPool.in_use,
		txPool.high_water,
		delta(txPool.exhausted, s_txPool0.exhausted),
		delta(txPool.rejected, s_txPool0.rejected),
	};
	for (const uint32_t value : values) {
		if (!writer.putU32(value)) {
			return false;
		}
	}

	putCounter(writer, usartIrq);
	putCounter(writer, rxDmaIrq);
	putCounter(writer, txDmaIrq);
	putCounter(writer, uartProbe.rx);
	putCounter(writer, uartProbe.slow);
	putCounter(writer, uartProbe.tx_start);
	putCounter(writer, cobsConsume);
	putCounter(writer, cobsTxRelease);
	putCounter(writer, packetProcess);
	return sendWriter(writer);
}

void runBackpressureSelfTest() noexcept
{
	constexpr std::array<uint8_t, 8> body{1u, 0u, 2u, 0u, 3u, 0u, 4u, 0u};
	auto second = s_link.make_message(body.size());
	if (!second || !second.append_bytes(body)) {
		++s_app.selftest_failures;
		return;
	}
	const cobs::SendResult result = s_link.send(second);
	if (result != cobs::SendResult::Busy || !second ||
			second.size() != body.size()) {
		++s_app.selftest_failures;
	}
	// The ACK owns one TX block and `second` owns the other. A third allocation
	// must fail cleanly without corrupting either owner.
	auto exhausted = s_link.make_message(1u);
	if (exhausted) {
		++s_app.selftest_failures;
	}
}

[[nodiscard]] bool queueAction(
	const PendingAction action, const uint32_t milliseconds) noexcept
{
	if (s_pendingAction != PendingAction::None) {
		return false;
	}
	s_pendingAction = action;
	s_pendingMs = milliseconds;
	return true;
}

[[nodiscard]] bool hasControlMagic(const std::span<const uint8_t> body) noexcept
{
	return body.size() >= 9u &&
		std::memcmp(body.data(), kMagic.data(), kMagic.size()) == 0;
}

void processControl(const std::span<const uint8_t> body) noexcept
{
	++s_app.control_frames;
	const Command command = static_cast<Command>(body[4]);
	const uint32_t token = readU32(body, 5u);
	const uint32_t argument = (body.size() >= 13u) ? readU32(body, 9u) : 0u;

	switch (command) {
	case Command::Hello:
		(void)sendHello(token);
		return;
	case Command::Stats:
		(void)sendStats(token);
		return;
	case Command::ResetMetrics:
		if (!sendAck(command, token, 0u, 0u)) {
			return;
		}
		(void)queueAction(PendingAction::ResetMetrics, 0u);
		return;
	case Command::HoldPackets:
	case Command::StallLoop: {
		const bool valid = body.size() >= 13u && argument >= 10u &&
			argument <= kMaxActionMs;
		const PendingAction action = (command == Command::HoldPackets)
			? PendingAction::HoldPackets : PendingAction::StallLoop;
		if (!valid || s_pendingAction != PendingAction::None) {
			(void)sendAck(command, token, 1u, argument);
			return;
		}
		if (sendAck(command, token, 0u, argument)) {
			(void)queueAction(action, argument);
		}
		return;
	}
	case Command::BackpressureSelfTest:
		if (sendAck(command, token, 0u, 0u)) {
			runBackpressureSelfTest();
		}
		return;
	}
	(void)sendAck(command, token, 2u, 0u);
}

void processOnePacket() noexcept
{
	if (s_link.tx_active() || !s_link.has_packet()) {
		return;
	}
	const uint32_t started = DWT->CYCCNT;
	{
		// Keep the packet in a nested scope so the measurement includes the
		// intrusive owner release and checked RX-pool return, not just the
		// application copy/encode/start portion in the middle.
		auto packet = s_link.pop_packet();
		if (packet) {
			const std::span<const uint8_t> body = packet.data();
			if (hasControlMagic(body)) {
				processControl(body);
			} else if (sendBody(body)) {
				++s_app.echo_frames;
				s_app.echo_bytes += static_cast<uint32_t>(body.size());
			}
		}
	}
	bench_counter_add(&s_packet_process, DWT->CYCCNT - started);
}

void pollLink() noexcept
{
	// Idle/in-flight polls stay completely uninstrumented: charging two DWT
	// reads to every spin would alter the scheduler-dependent fast check being
	// observed.  Measure the one poll that actually returns the borrowed TX
	// block to its checked pool.  If completion races an observed busy state,
	// leave Endpoint active and measure the release on the following loop.
	if (!s_link.tx_active() || s_uart.tx_busy()) {
		return;
	}
	const uint32_t started = DWT->CYCCNT;
	s_link.poll();
	bench_counter_add(&s_cobs_tx_release, DWT->CYCCNT - started);
}

void applyPendingAction(const uint32_t now) noexcept
{
	if (s_link.tx_active() || s_pendingAction == PendingAction::None) {
		return;
	}
	const PendingAction action = s_pendingAction;
	const uint32_t milliseconds = s_pendingMs;
	s_pendingAction = PendingAction::None;
	s_pendingMs = 0u;

	switch (action) {
	case PendingAction::ResetMetrics:
		resetMetrics();
		break;
	case PendingAction::HoldPackets:
		s_holdUntil = now + milliseconds;
		s_holdActive = true;
		break;
	case PendingAction::StallLoop:
		s_stallUntil = now + milliseconds;
		s_stallActive = true;
		break;
	case PendingAction::None:
		break;
	}
}

struct Transport final {
	bool send(const std::span<const uint8_t> frame) noexcept
	{
		return s_uart.send(frame);
	}

	[[nodiscard]] bool busy() const noexcept { return s_uart.tx_busy(); }
};

Transport s_transport;

void onRx(const std::span<const uint8_t> bytes) noexcept
{
	const uint32_t started = DWT->CYCCNT;
	s_link.consume(bytes);
	bench_counter_add(&s_cobs_consume, DWT->CYCCNT - started);
}

} // namespace

extern "C" void bench_init(void)
{
	SCB_EnableICache();
	SCB_EnableDCache();
	uart_probe::init();

	huart3.Init.BaudRate = COBS_HW_BAUD;
	huart3.Init.OverSampling = (COBS_HW_BAUD > 9000000u)
		? UART_OVERSAMPLING_8 : UART_OVERSAMPLING_16;
	if (HAL_UART_Init(&huart3) != HAL_OK ||
		HAL_UARTEx_SetTxFifoThreshold(&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK ||
		HAL_UARTEx_SetRxFifoThreshold(&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK ||
		HAL_UARTEx_EnableFifoMode(&huart3) != HAL_OK) {
		Error_Handler();
	}

	s_uart.setRxHandler([](const std::span<const uint8_t> bytes) noexcept {
		onRx(bytes);
	});
	s_uart.setRxGapHandler([]() noexcept { s_link.notify_gap(); });

	if (!s_link.bind(
			Link::Sender{tiny::bind<&Transport::send>(s_transport)},
			Link::BusyQuery{tiny::bind<&Transport::busy>(s_transport)}) ||
			!s_uart.init(&huart3)) {
		Error_Handler();
	}
	resetMetrics();
}

extern "C" void bench_loop(void)
{
	uint32_t now = HAL_GetTick();
	if (s_stallActive && deadlinePending(now, s_stallUntil)) {
		return; // IRQ/DMA continue; this deliberately starves the loop consumer
	}
	if (s_stallActive) {
		s_stallActive = false;
	}

	s_uart.proceed(now);
	pollLink();
	applyPendingAction(now);

	if (s_stallActive && deadlinePending(now, s_stallUntil)) {
		return;
	}
	if (s_holdActive && deadlinePending(now, s_holdUntil)) {
		return; // consume COBS bytes, but retain ready packets to exhaust RX pool
	}
	if (s_holdActive) {
		s_holdActive = false;
	}
	processOnePacket();
}
