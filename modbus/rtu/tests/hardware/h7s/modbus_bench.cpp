/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * End-to-end Modbus RTU + UART hardware harness for NUCLEO-H7S3L8.
 *
 * The Cube scaffold supplies USART3, GPDMA, instrumented IRQ handlers, and
 * calls bench_init()/bench_loop(). Every request and response below travels
 * through the production stack:
 *
 *   independent PC RTU codec <-> ST-Link VCP <-> USART3/GPDMA
 *                            <-> Uart<256,4>
 *                            <-> Endpoint<Pool<8,2>, selected CRC policy>
 *
 * Ordinary CRC-valid ADUs are echoed with the same address, function and
 * function data. A reserved address/function/data envelope carries harness
 * control commands through the same Packet/Message/CRC/Pool path.
 */

#define UART_ENGINE_PROBE 1
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"

#include "modbus/rtu/Rtu.h"
#include "uart_bench.h"
#include "usart.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#ifndef MODBUS_HW_BAUD
#define MODBUS_HW_BAUD 115200u
#endif
#ifndef MODBUS_HW_CRC_POLICY_ID
#define MODBUS_HW_CRC_POLICY_ID 0
#endif

static_assert(MODBUS_HW_BAUD > 0u, "Modbus hardware baud must be non-zero");
static_assert(MODBUS_HW_CRC_POLICY_ID >= 0 && MODBUS_HW_CRC_POLICY_ID <= 8,
	"MODBUS_HW_CRC_POLICY_ID must select one supported built-in policy");

BenchCounter g_bench_usart_irq;
BenchCounter g_bench_rx_dma_irq;
BenchCounter g_bench_tx_dma_irq;

namespace {

constexpr std::size_t kRxBlocks = 8u;
constexpr std::size_t kTxBlocks = 2u;
constexpr std::size_t kUartChunkSize = 256u;
constexpr std::size_t kUartChunkCount = 4u;

using Memory = modbus::rtu::Pool<kRxBlocks, kTxBlocks>;
#if MODBUS_HW_CRC_POLICY_ID == 0
using Crc = ::crc::Crc16Bitwise;
constexpr uint32_t kCrcPolicy = 0u;
#elif MODBUS_HW_CRC_POLICY_ID == 1
using Crc = modbus::rtu::crc::Table;
constexpr uint32_t kCrcPolicy = 1u;
#elif MODBUS_HW_CRC_POLICY_ID == 2
using Crc = ::crc::NoCrc;
constexpr uint32_t kCrcPolicy = 2u;
#elif MODBUS_HW_CRC_POLICY_ID == 3
using Crc = ::crc::Crc8Bitwise;
constexpr uint32_t kCrcPolicy = 3u;
#elif MODBUS_HW_CRC_POLICY_ID == 4
using Crc = ::crc::Crc8Table;
constexpr uint32_t kCrcPolicy = 4u;
#elif MODBUS_HW_CRC_POLICY_ID == 5
using Crc = ::crc::Crc32Bitwise;
constexpr uint32_t kCrcPolicy = 5u;
#elif MODBUS_HW_CRC_POLICY_ID == 6
using Crc = ::crc::Crc32Table;
constexpr uint32_t kCrcPolicy = 6u;
#elif MODBUS_HW_CRC_POLICY_ID == 7
using Crc = ::crc::Crc64Bitwise;
constexpr uint32_t kCrcPolicy = 7u;
#else
using Crc = ::crc::Crc64Table;
constexpr uint32_t kCrcPolicy = 8u;
#endif
using Link = modbus::rtu::Endpoint<Memory, Crc>;
using Serial = Uart<kUartChunkSize, kUartChunkCount>;

static_assert(Link::max_receive_size ==
	modbus::rtu::max_adu_size - 2u - Crc::wire_size);
static_assert(Link::max_send_size == Link::max_receive_size);
static_assert(Link::max_frame_size == 256u);

constexpr uint8_t kControlAddress = 0xF7u;
constexpr uint8_t kControlFunction = 0x41u;
constexpr std::array<uint8_t, 4> kMagic{0x4Du, 0x52u, 0x54u, 0x55u};
constexpr uint32_t kProtocolVersion = 3u;
constexpr uint32_t kMaxActionMs = 5000u;
constexpr std::size_t kControlPrefixSize = 9u;
constexpr std::size_t kStatsScalarCount = 31u;
constexpr std::size_t kCounterCount = 7u;
constexpr std::size_t kCounterWireSize = 16u;
constexpr std::size_t kStatsDataSize = kControlPrefixSize +
	(kStatsScalarCount * sizeof(uint32_t)) +
	(kCounterCount * kCounterWireSize);
static_assert(kStatsDataSize == 245u);
static_assert(kStatsDataSize <= Link::max_send_size);

enum class Command : uint8_t {
	Hello = 1u,
	Stats = 2u,
	ResetMetrics = 3u,
	HoldPackets = 4u,
	BackpressureSelfTest = 5u,
	CrcBenchmark = 6u,
};

enum class PendingAction : uint8_t { None, ResetMetrics, HoldPackets };

Serial s_uart;
Link s_link;

BenchCounter s_rtu_receive;
BenchCounter s_rtu_tx_release;
BenchCounter s_packet_process;

struct AppMetrics final {
	uint32_t echo_frames = 0u;
	uint32_t echo_data_bytes = 0u;
	uint32_t control_frames = 0u;
	uint32_t response_failures = 0u;
	uint32_t selftest_failures = 0u;
};

AppMetrics s_app;
modbus::rtu::Stats s_rtu0;
Serial::Stats s_uart0;
Memory::PoolStats s_rx_pool0;
Memory::PoolStats s_tx_pool0;
uint32_t s_window_start = 0u;

PendingAction s_pending_action = PendingAction::None;
uint32_t s_pending_ms = 0u;
uint32_t s_hold_until = 0u;
bool s_hold_active = false;

#if defined(__GNUC__)
#define MODBUS_BENCH_NOINLINE __attribute__((noipa, used))
#else
#define MODBUS_BENCH_NOINLINE
#endif

/*
 * Stable symbol for both live DWT timing and exact linked-ELF disassembly.
 * The empty compiler barrier prevents loop-invariant call elimination without
 * adding an instruction to the target hot path.
 */
extern "C" MODBUS_BENCH_NOINLINE Crc::value_type modbus_crc_benchmark_probe(
		const uint8_t* const bytes,
		const std::size_t size) noexcept
{
#if defined(__GNUC__)
	asm volatile("" : : "r"(bytes), "r"(size) : "memory");
#endif
	Crc policy{};
	return policy.calculate(std::span<const uint8_t>{bytes, size});
}

#undef MODBUS_BENCH_NOINLINE

volatile uint64_t s_crc_benchmark_sink = 0u;

struct CrcBenchmarkResult final {
	uint64_t cycles = 0u;
	uint64_t checksum_mix = 0u;
	uint64_t checksum = 0u;
};

[[nodiscard]] CrcBenchmarkResult benchmark_crc(
		const std::span<const uint8_t> bytes,
		const uint32_t iterations) noexcept
{
	CrcBenchmarkResult result;
	/* Warm instruction/data cache before the timed hot loop. */
	result.checksum = modbus_crc_benchmark_probe(bytes.data(), bytes.size());

	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	__DSB();
	__ISB();
	const uint32_t started = DWT->CYCCNT;
	uint64_t checksum_mix = 0u;
	for (uint32_t iteration = 0u; iteration < iterations; ++iteration) {
		checksum_mix += static_cast<uint64_t>(
			modbus_crc_benchmark_probe(bytes.data(), bytes.size())) ^ iteration;
	}
	// Keep the complete call/mix loop between the two timer reads.
	asm volatile("" : "+r"(checksum_mix) : : "memory");
	const uint32_t stopped = DWT->CYCCNT;
	__set_PRIMASK(primask);

	s_crc_benchmark_sink = checksum_mix;
	result.cycles = static_cast<uint64_t>(stopped - started);
	result.checksum_mix = checksum_mix;
	return result;
}

class Writer final {
public:
	[[nodiscard]] bool put_u8(const uint8_t value) noexcept
	{
		if (m_size == m_bytes.size()) {
			return false;
		}
		m_bytes[m_size++] = value;
		return true;
	}

	[[nodiscard]] bool put_u32(const uint32_t value) noexcept
	{
		for (unsigned shift = 0u; shift < 32u; shift += 8u) {
			if (!put_u8(static_cast<uint8_t>(value >> shift))) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool put_u64(const uint64_t value) noexcept
	{
		for (unsigned shift = 0u; shift < 64u; shift += 8u) {
			if (!put_u8(static_cast<uint8_t>(value >> shift))) {
				return false;
			}
		}
		return true;
	}

	[[nodiscard]] bool put_bytes(
			const std::span<const uint8_t> bytes) noexcept
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
	std::array<uint8_t, Link::max_send_size> m_bytes{};
	std::size_t m_size = 0u;
};

[[nodiscard]] uint32_t read_le_u32(
		const std::span<const uint8_t> bytes,
		const std::size_t offset) noexcept
{
	std::size_t cursor = offset;
	uint32_t value = 0u;
	return modbus::read_le(bytes, cursor, value) ? value : 0u;
}

[[nodiscard]] uint32_t delta(
		const uint32_t value, const uint32_t baseline) noexcept
{
	return value - baseline;
}

[[nodiscard]] bool deadline_pending(
		const uint32_t now, const uint32_t deadline) noexcept
{
	return static_cast<int32_t>(now - deadline) < 0;
}

template<class Counter>
[[nodiscard]] bool put_counter(
		Writer& writer, const Counter& counter) noexcept
{
	return writer.put_u64(counter.total) &&
		writer.put_u32(counter.calls) &&
		writer.put_u32(counter.max);
}

[[nodiscard]] bool begin_response(
		Writer& writer,
		const Command command,
		const uint32_t token) noexcept
{
	return writer.put_bytes(kMagic) &&
		writer.put_u8(static_cast<uint8_t>(command) | 0x80u) &&
		writer.put_u32(token);
}

[[nodiscard]] bool send_data(
		const uint8_t address,
		const uint8_t function,
		const std::span<const uint8_t> data) noexcept
{
	auto message = s_link.make_message(address, function, data.size());
	if (!message || !message.append_bytes(data) ||
			s_link.send(message) != modbus::SendResult::Sent) {
		++s_app.response_failures;
		return false;
	}
	return true;
}

[[nodiscard]] bool send_writer(const Writer& writer) noexcept
{
	return send_data(kControlAddress, kControlFunction, writer.data());
}

void reset_metrics() noexcept
{
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	s_rtu0 = s_link.stats();
	s_uart0 = s_uart.stats();
	s_rx_pool0 = s_link.storage().rx_stats();
	s_tx_pool0 = s_link.storage().tx_stats();
	s_app = {};
	s_window_start = HAL_GetTick();

	g_bench_usart_irq = {};
	g_bench_rx_dma_irq = {};
	g_bench_tx_dma_irq = {};
	uart_probe::g_stats = {};
	s_rtu_receive = {};
	s_rtu_tx_release = {};
	s_packet_process = {};
	__set_PRIMASK(primask);
}

[[nodiscard]] bool send_ack(
		const Command command,
		const uint32_t token,
		const uint32_t status,
		const uint32_t value) noexcept
{
	Writer writer;
	return begin_response(writer, command, token) &&
		writer.put_u32(status) && writer.put_u32(value) &&
		send_writer(writer);
}

[[nodiscard]] bool send_hello(const uint32_t token) noexcept
{
	Writer writer;
	return begin_response(writer, Command::Hello, token) &&
		writer.put_u32(kProtocolVersion) &&
		writer.put_u32(MODBUS_HW_BAUD) &&
		writer.put_u32(SystemCoreClock) &&
		writer.put_u32(static_cast<uint32_t>(Link::max_receive_size)) &&
		writer.put_u32(static_cast<uint32_t>(Link::max_send_size)) &&
		writer.put_u32(static_cast<uint32_t>(Link::max_frame_size)) &&
		writer.put_u32(static_cast<uint32_t>(kUartChunkSize)) &&
		writer.put_u32(static_cast<uint32_t>(kUartChunkCount)) &&
		writer.put_u32(static_cast<uint32_t>(kRxBlocks)) &&
		writer.put_u32(static_cast<uint32_t>(kTxBlocks)) &&
		writer.put_u32(kCrcPolicy) &&
		send_writer(writer);
}

[[nodiscard]] bool send_crc_benchmark(
		const uint32_t token,
		const uint32_t size,
		const uint32_t iterations) noexcept
{
	if (size > 256u || iterations == 0u || iterations > 8u) {
		return send_ack(Command::CrcBenchmark, token, 1u, size);
	}

	std::array<uint8_t, 256u> bytes{};
	for (std::size_t index = 0u; index < size; ++index) {
		bytes[index] = static_cast<uint8_t>(
			(index * 37u + 0xA5u) & 0xFFu);
	}
	const CrcBenchmarkResult result = benchmark_crc(
		std::span<const uint8_t>{bytes}.first(size), iterations);

	Writer writer;
	return begin_response(writer, Command::CrcBenchmark, token) &&
		writer.put_u32(0u) &&
		writer.put_u32(size) &&
		writer.put_u32(iterations) &&
		writer.put_u64(result.cycles) &&
		writer.put_u64(result.checksum_mix) &&
		writer.put_u64(result.checksum) &&
		writer.put_u32(DWT->CTRL) &&
		writer.put_u32(SCB->CCR) &&
		writer.put_u32(SCB->CPUID) &&
		send_writer(writer);
}

[[nodiscard]] bool send_stats(const uint32_t token) noexcept
{
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	const modbus::rtu::Stats rtu_stats = s_link.stats();
	const Serial::Stats uart_stats = s_uart.stats();
	const Memory::PoolStats rx_pool = s_link.storage().rx_stats();
	const Memory::PoolStats tx_pool = s_link.storage().tx_stats();
	const AppMetrics app = s_app;
	const uint32_t window_ms = HAL_GetTick() - s_window_start;
	const BenchCounter usart_irq = g_bench_usart_irq;
	const BenchCounter rx_dma_irq = g_bench_rx_dma_irq;
	const BenchCounter tx_dma_irq = g_bench_tx_dma_irq;
	const uart_probe::Stats uart_probe_stats = uart_probe::g_stats;
	const BenchCounter rtu_receive = s_rtu_receive;
	const BenchCounter packet_process = s_packet_process;
	const BenchCounter rtu_tx_release = s_rtu_tx_release;
	__set_PRIMASK(primask);

	Writer writer;
	if (!begin_response(writer, Command::Stats, token)) {
		return false;
	}

	const std::array<uint32_t, kStatsScalarCount> values{
		kProtocolVersion,
		window_ms,
		app.echo_frames,
		app.echo_data_bytes,
		app.control_frames,
		app.response_failures,
		app.selftest_failures,
		delta(uart_stats.rx_overrun, s_uart0.rx_overrun),
		delta(uart_stats.rx_errors, s_uart0.rx_errors),
		delta(uart_stats.tx_errors, s_uart0.tx_errors),
		delta(uart_stats.restarts, s_uart0.restarts),
		delta(rtu_stats.rx.candidates, s_rtu0.rx.candidates),
		delta(rtu_stats.rx.frames_received, s_rtu0.rx.frames_received),
		delta(rtu_stats.rx.crc_errors, s_rtu0.rx.crc_errors),
		delta(rtu_stats.rx.too_short, s_rtu0.rx.too_short),
		delta(rtu_stats.rx.oversize, s_rtu0.rx.oversize),
		delta(rtu_stats.rx.allocation_failure, s_rtu0.rx.allocation_failure),
		delta(rtu_stats.rx.stream_gaps, s_rtu0.rx.stream_gaps),
		delta(rtu_stats.tx.frames_sent, s_rtu0.tx.frames_sent),
		delta(rtu_stats.tx.send_refused_busy,
		      s_rtu0.tx.send_refused_busy),
		delta(rtu_stats.tx.send_failed, s_rtu0.tx.send_failed),
		static_cast<uint32_t>(kRxBlocks) - rx_pool.in_use,
		rx_pool.in_use,
		rx_pool.high_water,
		delta(rx_pool.exhausted, s_rx_pool0.exhausted),
		delta(rx_pool.rejected, s_rx_pool0.rejected),
		static_cast<uint32_t>(kTxBlocks) - tx_pool.in_use,
		tx_pool.in_use,
		tx_pool.high_water,
		delta(tx_pool.exhausted, s_tx_pool0.exhausted),
		delta(tx_pool.rejected, s_tx_pool0.rejected),
	};
	for (const uint32_t value : values) {
		if (!writer.put_u32(value)) {
			return false;
		}
	}

	return put_counter(writer, usart_irq) &&
		put_counter(writer, rx_dma_irq) &&
		put_counter(writer, tx_dma_irq) &&
		put_counter(writer, uart_probe_stats.slow) &&
		put_counter(writer, rtu_receive) &&
		put_counter(writer, packet_process) &&
		put_counter(writer, rtu_tx_release) &&
		writer.data().size() == kStatsDataSize &&
		send_writer(writer);
}

void run_backpressure_selftest() noexcept
{
	auto contender = s_link.make_message(1u, 0x64u, 2u);
	if (!contender || !contender.append_be(uint16_t{0x1234u})) {
		++s_app.selftest_failures;
		return;
	}
	const modbus::SendResult result = s_link.send(contender);
	if (result != modbus::SendResult::Busy || !contender ||
			contender.size() != 2u || !contender.append_native(uint8_t{0xA5u})) {
		++s_app.selftest_failures;
	}

	// The ACK owns one TX block and contender owns the second. The third
	// allocation must fail without stealing or corrupting either owner.
	auto exhausted = s_link.make_message(1u, 0x64u, 1u);
	if (exhausted) {
		++s_app.selftest_failures;
	}
}

[[nodiscard]] bool queue_action(
		const PendingAction action,
		const uint32_t milliseconds) noexcept
{
	if (s_pending_action != PendingAction::None) {
		return false;
	}
	s_pending_action = action;
	s_pending_ms = milliseconds;
	return true;
}

[[nodiscard]] bool has_control_magic(const Link::Packet& packet) noexcept
{
	const std::span<const uint8_t> data = packet.data();
	return packet.address() == kControlAddress &&
		packet.function() == kControlFunction &&
		data.size() >= kControlPrefixSize &&
		std::memcmp(data.data(), kMagic.data(), kMagic.size()) == 0;
}

void process_control(const std::span<const uint8_t> data) noexcept
{
	++s_app.control_frames;
	const Command command = static_cast<Command>(data[4]);
	const uint32_t token = read_le_u32(data, 5u);
	const uint32_t argument = data.size() >= 13u ? read_le_u32(data, 9u) : 0u;
	const uint32_t second_argument =
		data.size() >= 17u ? read_le_u32(data, 13u) : 0u;

	switch (command) {
	case Command::Hello:
		(void)send_hello(token);
		return;
	case Command::Stats:
		(void)send_stats(token);
		return;
	case Command::ResetMetrics:
		if (send_ack(command, token, 0u, 0u)) {
			(void)queue_action(PendingAction::ResetMetrics, 0u);
		}
		return;
	case Command::HoldPackets: {
		const bool valid = data.size() >= 13u && argument >= 10u &&
			argument <= kMaxActionMs;
		if (!valid || s_pending_action != PendingAction::None) {
			(void)send_ack(command, token, 1u, argument);
			return;
		}
		if (send_ack(command, token, 0u, argument)) {
			(void)queue_action(PendingAction::HoldPackets, argument);
		}
		return;
	}
	case Command::BackpressureSelfTest:
		if (send_ack(command, token, 0u, 0u)) {
			run_backpressure_selftest();
		}
		return;
	case Command::CrcBenchmark:
		if (data.size() < 17u) {
			(void)send_ack(command, token, 1u, 0u);
			return;
		}
		(void)send_crc_benchmark(token, argument, second_argument);
		return;
	}
	(void)send_ack(command, token, 2u, 0u);
}

void process_one_packet() noexcept
{
	if (s_link.tx_active() || !s_link.has_packet()) {
		return;
	}
	const uint32_t started = DWT->CYCCNT;
	{
		auto packet = s_link.pop_packet();
		if (packet) {
			if (has_control_magic(packet)) {
				process_control(packet.data());
			} else if (send_data(packet.address(), packet.function(),
			                          packet.data())) {
				++s_app.echo_frames;
				s_app.echo_data_bytes +=
					static_cast<uint32_t>(packet.size());
			}
		}
	}
	bench_counter_add(&s_packet_process, DWT->CYCCNT - started);
}

void poll_link() noexcept
{
	if (!s_link.tx_active() || s_uart.tx_busy()) {
		return;
	}
	const uint32_t started = DWT->CYCCNT;
	s_link.poll();
	bench_counter_add(&s_rtu_tx_release, DWT->CYCCNT - started);
}

void apply_pending_action(const uint32_t now) noexcept
{
	if (s_link.tx_active() || s_pending_action == PendingAction::None) {
		return;
	}
	const PendingAction action = s_pending_action;
	const uint32_t milliseconds = s_pending_ms;
	s_pending_action = PendingAction::None;
	s_pending_ms = 0u;

	switch (action) {
	case PendingAction::ResetMetrics:
		reset_metrics();
		break;
	case PendingAction::HoldPackets:
		s_hold_until = now + milliseconds;
		s_hold_active = true;
		break;
	case PendingAction::None:
		break;
	}
}

struct Transport final {
	[[nodiscard]] bool send(
			const std::span<const uint8_t> frame) noexcept
	{
		return s_uart.send(frame);
	}

	[[nodiscard]] bool busy() const noexcept { return s_uart.tx_busy(); }
};

Transport s_transport;

void on_rx(const std::span<const uint8_t> bytes) noexcept
{
	const uint32_t started = DWT->CYCCNT;
	s_link.receive_adu(bytes);
	bench_counter_add(&s_rtu_receive, DWT->CYCCNT - started);
}

} // namespace

extern "C" void bench_init(void)
{
	SCB_EnableICache();
	SCB_EnableDCache();
	uart_probe::init();

	huart3.Init.BaudRate = MODBUS_HW_BAUD;
	huart3.Init.OverSampling = (MODBUS_HW_BAUD > 9000000u)
		? UART_OVERSAMPLING_8 : UART_OVERSAMPLING_16;
	if (HAL_UART_Init(&huart3) != HAL_OK ||
			HAL_UARTEx_SetTxFifoThreshold(
				&huart3, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK ||
			HAL_UARTEx_SetRxFifoThreshold(
				&huart3, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK ||
			HAL_UARTEx_EnableFifoMode(&huart3) != HAL_OK) {
		Error_Handler();
	}

	s_uart.setRxHandler([](const std::span<const uint8_t> bytes) noexcept {
		on_rx(bytes);
	});
	s_uart.setRxGapHandler([]() noexcept { s_link.notify_gap(); });

	if (!s_link.bind(
			Link::Sender{tiny::bind<&Transport::send>(s_transport)},
			Link::BusyQuery{tiny::bind<&Transport::busy>(s_transport)}) ||
			!s_uart.init(&huart3)) {
		Error_Handler();
	}
	reset_metrics();
}

extern "C" void bench_loop(void)
{
	const uint32_t now = HAL_GetTick();
	s_uart.proceed(now);
	poll_link();
	apply_pending_action(now);

	if (s_hold_active && deadline_pending(now, s_hold_until)) {
		return; // receive into the Endpoint while retaining every ready Packet
	}
	if (s_hold_active) {
		s_hold_active = false;
	}
	process_one_packet();
}
