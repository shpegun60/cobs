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
 *                            <-> Endpoint<Pool<8,2>, selected CRC policy
 *                                         [, HarnessFramer]>
 *
 * MODBUS_HW_FRAMER=1 selects the optional framing policy: every function of
 * the harness protocol becomes length-prefixed ([N: BE16][body]) so frame
 * ends are found from the bytes, and the UART callback feeds arbitrary
 * chunks to consume() instead of one burst candidate to receive_adu(). HELLO
 * reports the mode and the PC peer prepends the same prefix.
 *
 * Ordinary CRC-valid ADUs are echoed with the same address, function and
 * function data. A reserved address/function/data envelope carries harness
 * control commands through the same Packet/Message/CRC/Pool path.
 *
 * MODBUS_HW_ROLE turns the harness into one side of a standard Modbus
 * exchange for the comparison against Qt's QtSerialBus on the PC
 * (src/adapters/qt/tests/hardware/h7s):
 *   1 — a SERVER at unit 0x11 serving modbus/rtu/tests/reference_model.h,
 *       for QModbusRtuSerialClient or this repository's RtuClient on the PC;
 *   2 — a CLIENT running a fixed script of requests against a
 *       QModbusRtuSerialServer at unit 0x0A on the PC, predicting every
 *       response from the same reference model kept as a shadow, and
 *       reporting each step's verdict and round-trip time.
 * In both roles the control envelope keeps working (it is how the PC starts
 * the client script and collects the report), and with MODBUS_HW_FRAMER=1
 * the framing table is the standard one for the role's direction plus the
 * control function; frames for other units are ignored, not echoed.
 */

#define UART_ENGINE_PROBE 1
#define UART_ENGINE_IMPLEMENT
#include "Uart.h"

#include "modbus/rtu/Rtu.h"
#include "modbus/rtu/tests/reference_model.h"
#include "adapters/rtu/UartAdapter.h"
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
#ifndef MODBUS_HW_FRAMER
#define MODBUS_HW_FRAMER 0
#endif
// MODBUS_HW_WAKE=1 installs the cheapest possible wake handler (a volatile
// increment) so the cost of the driver's ISR-side wake call can be measured
// against an image without a handler; a real RTOS notification adds its own,
// scheduler-defined cost on top.
#ifndef MODBUS_HW_WAKE
#define MODBUS_HW_WAKE 0
#endif
#ifndef MODBUS_HW_ROLE
#define MODBUS_HW_ROLE 0
#endif
static_assert(MODBUS_HW_FRAMER == 0 || MODBUS_HW_FRAMER == 1,
	"MODBUS_HW_FRAMER selects the framing policy: 0 or 1");
static_assert(MODBUS_HW_ROLE >= 0 && MODBUS_HW_ROLE <= 2,
	"MODBUS_HW_ROLE: 0 echo harness, 1 reference server, 2 scripted client");
// Data bytes owned by the framing policy in front of the CONTROL body (in the
// echo role, in front of every body).
constexpr std::size_t kFramePrefix = MODBUS_HW_FRAMER ? 2u : 0u;
constexpr uint8_t kControlFunction = 0x41u;

// The prefix a given function carries: in the echo role every function is
// length-prefixed; in the server and client roles the standard functions keep
// their standard layout and only the harness's control function is prefixed.
[[nodiscard]] constexpr std::size_t prefix_for(const uint8_t function) noexcept
{
#if MODBUS_HW_FRAMER
	return (MODBUS_HW_ROLE == 0 || function == kControlFunction) ? 2u : 0u;
#else
	(void)function;
	return 0u;
#endif
}
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

using Memory = wire::Pool<kRxBlocks, kTxBlocks>;
#if MODBUS_HW_CRC_POLICY_ID == 0
using Crc = ::crc::Crc16Bitwise;
constexpr uint32_t kCrcPolicy = 0u;
#elif MODBUS_HW_CRC_POLICY_ID == 1
using Crc = ::crc::Crc16Table;
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
#if MODBUS_HW_FRAMER && MODBUS_HW_ROLE == 0
// The harness protocol is not standard Modbus: every function carries an
// arbitrary body, so every function is declared length-prefixed. Direction is
// irrelevant to such a table but the policy contract still names one.
struct HarnessFramer {
	static constexpr modbus::rtu::framing::Direction rx =
		modbus::rtu::framing::Direction::Request;

	[[nodiscard]] static constexpr modbus::rtu::framing::Layout layout(
			modbus::rtu::framing::Direction,
			uint8_t) noexcept
	{
		return modbus::rtu::framing::Layout::length_prefixed(2u);
	}
};
using Framer = HarnessFramer;
#elif MODBUS_HW_FRAMER
// A standard Modbus side: the standard table for what this role receives
// (a server receives requests, a client responses), plus the harness's own
// control function as a length-prefixed private function.
struct RoleFramer : modbus::rtu::framing::Standard<MODBUS_HW_ROLE == 1
		? modbus::rtu::framing::Direction::Request
		: modbus::rtu::framing::Direction::Response> {
	using Base = modbus::rtu::framing::Standard<rx>;

	[[nodiscard]] static constexpr modbus::rtu::framing::Layout layout(
			const modbus::rtu::framing::Direction direction,
			const uint8_t function) noexcept
	{
		return function == kControlFunction
			? modbus::rtu::framing::Layout::length_prefixed(2u)
			: Base::layout(direction, function);
	}
};
using Framer = RoleFramer;
#else
using Framer = modbus::rtu::framing::None;
#endif
using Link = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<Crc>, Framer>;
using Serial = Uart<kUartChunkSize, kUartChunkCount>;

static_assert(Link::max_receive_size ==
	modbus::rtu::standard_adu_size - 2u - Crc::wire_size);
static_assert(Link::max_send_size == Link::max_receive_size);
static_assert(Link::max_frame_size == 256u);

constexpr uint8_t kControlAddress = 0xF7u;
constexpr std::array<uint8_t, 4> kMagic{0x4Du, 0x52u, 0x54u, 0x55u};
// Harness protocol, not Modbus: version 4 appends the framing mode to HELLO.
constexpr uint32_t kProtocolVersion = 4u;
constexpr uint32_t kMaxActionMs = 5000u;
constexpr std::size_t kControlPrefixSize = 9u;
constexpr std::size_t kStatsScalarCount = 31u;
constexpr std::size_t kCounterCount = 7u;
constexpr std::size_t kCounterWireSize = 16u;
constexpr std::size_t kStatsDataSize = kControlPrefixSize +
	(kStatsScalarCount * sizeof(uint32_t)) +
	(kCounterCount * kCounterWireSize);
static_assert(kStatsDataSize == 245u);
static_assert(kStatsDataSize + kFramePrefix <= Link::max_send_size);

enum class Command : uint8_t {
	Hello = 1u,
	Stats = 2u,
	ResetMetrics = 3u,
	HoldPackets = 4u,
	BackpressureSelfTest = 5u,
	CrcBenchmark = 6u,
	RoleReport = 7u,    // argument: page; the role's counters and the client's step verdicts
	StartClient = 8u,   // argument: delay in ms before the client script starts
	ResetModel = 9u,    // the served (or shadowed) reference model back to its initial contents
};

// RoleReport payload: an 8-byte header, four 32-bit role counters, and up to
// kReportEntries 8-byte step entries (client role; zero in the server role).
constexpr std::size_t kReportEntries = 27u;
constexpr std::size_t kReportDataSize = 8u + 4u * sizeof(uint32_t) + kReportEntries * 8u;
static_assert(kReportDataSize == 240u);

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

// Role counters, reported by RoleReport in this order.
struct RoleCounters final {
	uint32_t served = 0u;        // server: responses sent | client: responses matched
	uint32_t exceptions = 0u;    // server: exception responses sent | client: mismatches
	uint32_t broadcasts = 0u;    // server: broadcast writes executed | client: timeouts
	uint32_t ignored = 0u;       // frames for other units (server) or unsolicited (client)
};
RoleCounters s_role;
modbus_reference::Model s_model;   // served (server) or shadowed (client)

#if MODBUS_HW_ROLE == 2
namespace client {

// QModbus's defaults, so the two clients compared on the PC and this one
// behave alike on the wire.
constexpr uint32_t kResponseTimeoutMs = 1000u;
constexpr uint32_t kInterFrameMs = 2u;
constexpr uint32_t kTurnaroundMs = 100u;
constexpr std::size_t kSteps = modbus_reference::script::kSteps;
using Step = modbus_reference::script::Request;

enum class Status : uint8_t {
	Ok = 0u,          // behaved as the reference model predicts (a response that matched, or silence where none was due)
	Mismatch = 1u,    // a response arrived and differed; detail = the function it carried
	Timeout = 2u,     // a response was due and none came within kResponseTimeoutMs
	Unexpected = 3u,  // a response arrived where none was due (broadcast, foreign unit)
	SendFailed = 4u,  // the endpoint or the transport refused the frame
	Refused = 5u,     // the framed builder refused the function before the wire (unknown function)
	Pending = 0xFFu,
};

struct Result final {
	Status status = Status::Pending;
	uint8_t detail = 0u;
	uint8_t responded = 0u;
	uint8_t reserved = 0u;
	uint32_t rtt_us = 0u;
};

struct Engine final {
	bool armed = false;
	uint32_t start_at = 0u;
	bool running = false;
	bool done = false;
	bool waiting = false;
	std::size_t index = 0u;
	uint32_t next_at = 0u;
	uint32_t sent_tick = 0u;
	uint32_t sent_cycles = 0u;
	Step step;
	modbus_reference::Reply expected;
	std::array<Result, kSteps> results{};
};
Engine s_engine;

} // namespace client
#endif
modbus::rtu::Stats s_rtu0;
Serial::Stats s_uart0;
Link::Storage::Stats s_rx_pool0;
Link::Storage::Stats s_tx_pool0;
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
	// With a framer the message already holds its reserved prefix; the hint
	// covers the body that follows it.
	auto message = s_link.make_message(address, function, data.size() + prefix_for(function));
	if (!message || !message.append_bytes(data) ||
			s_link.send(message) != modbus::SendResult::Sent) {
		++s_app.response_failures;
		return false;
	}
	return true;
}

#if MODBUS_HW_ROLE == 2
// Like send_data(), but tells a builder refusal (the framed endpoint will not
// send a function its table does not know) apart from a transport failure.
enum class SendOutcome : uint8_t { Sent, Refused, Failed };

[[nodiscard]] SendOutcome send_request(
		const uint8_t address,
		const uint8_t function,
		const std::span<const uint8_t> data) noexcept
{
	auto message = s_link.make_message(address, function, data.size() + prefix_for(function));
	if (!message || !message.append_bytes(data)) {
		return SendOutcome::Refused;
	}
	const modbus::SendResult result = s_link.send(message);
	if (result == modbus::SendResult::Sent) {
		return SendOutcome::Sent;
	}
	return result == modbus::SendResult::Invalid ? SendOutcome::Refused : SendOutcome::Failed;
}

[[nodiscard]] std::span<const uint8_t> body_of(const Link::Packet& packet) noexcept;
#endif

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

#if MODBUS_HW_ROLE == 2
namespace client {

void start(const uint32_t now, const uint32_t delay_ms) noexcept
{
	s_model.reset();
	s_engine = Engine{};
	s_engine.armed = true;
	s_engine.start_at = now + delay_ms;
}

void advance(const uint32_t now) noexcept
{
	s_engine.waiting = false;
	const uint32_t gap = s_engine.step.address == 0u ? kTurnaroundMs : kInterFrameMs;
	++s_engine.index;
	s_engine.next_at = now + gap;
}

[[nodiscard]] uint32_t round_trip_us() noexcept
{
	return (DWT->CYCCNT - s_engine.sent_cycles) / (SystemCoreClock / 1000000u);
}

void record(const Status status, const uint8_t detail, const bool responded, const uint32_t rtt_us) noexcept
{
	Result& result = s_engine.results[s_engine.index];
	result.status = status;
	result.detail = detail;
	result.responded = responded ? 1u : 0u;
	result.rtt_us = rtt_us;
	switch (status) {
	case Status::Ok: ++s_role.served; break;
	case Status::Mismatch: ++s_role.exceptions; break;
	case Status::Timeout: ++s_role.broadcasts; break;
	default: break;
	}
}

void step(const uint32_t now) noexcept
{
	if (s_engine.armed && !deadline_pending(now, s_engine.start_at)) {
		s_engine.armed = false;
		s_engine.running = true;
		s_engine.index = 0u;
		s_engine.next_at = now;
	}
	if (!s_engine.running) {
		return;
	}
	if (s_engine.waiting) {
		// Silence is the expected outcome for a broadcast and for a foreign
		// unit; it is a timeout when a response was due. Either way the wait
		// is QModbus's: the response timeout, or the turnaround after a
		// broadcast.
		const bool broadcast = s_engine.step.address == 0u;
		const uint32_t limit = broadcast ? kTurnaroundMs : kResponseTimeoutMs;
		if (!deadline_pending(now, s_engine.sent_tick + limit)) {
			record(s_engine.expected.respond ? Status::Timeout : Status::Ok, 0u, false, limit * 1000u);
			advance(now);
		}
		return;
	}
	if (s_engine.index >= kSteps) {
		s_engine.running = false;
		s_engine.done = true;
		return;
	}
	if (deadline_pending(now, s_engine.next_at) || s_link.tx_active()) {
		return;
	}
	modbus_reference::script::build(s_engine.index, modbus_reference::kPcUnit, s_engine.step);
	s_engine.expected = modbus_reference::serve(s_model, modbus_reference::kPcUnit,
		s_engine.step.address, s_engine.step.function, s_engine.step.span());
	s_engine.sent_cycles = DWT->CYCCNT;
	s_engine.sent_tick = now;
	switch (send_request(s_engine.step.address, s_engine.step.function, s_engine.step.span())) {
	case SendOutcome::Sent:
		s_engine.waiting = true;
		break;
	case SendOutcome::Refused:
		record(Status::Refused, 0u, false, 0u);
		advance(now);
		break;
	case SendOutcome::Failed:
		record(Status::SendFailed, 0u, false, 0u);
		advance(now);
		break;
	}
}

void on_packet(const Link::Packet& packet) noexcept
{
	if (!s_engine.running || !s_engine.waiting) {
		++s_role.ignored;   // unsolicited
		return;
	}
	const uint32_t rtt = round_trip_us();
	if (!s_engine.expected.respond) {
		record(Status::Unexpected, packet.function(), true, rtt);
		advance(HAL_GetTick());
		return;
	}
	if (packet.address() != s_engine.step.address) {
		++s_role.ignored;   // QModbus ignores a response from another server too
		return;
	}
	const std::span<const uint8_t> body = body_of(packet);
	const std::span<const uint8_t> want = s_engine.expected.span();
	bool same = packet.function() == s_engine.expected.function && body.size() == want.size();
	uint8_t detail = packet.function();
	if (same) {
		for (std::size_t i = 0u; i < body.size(); ++i) {
			if (body[i] != want[i]) {
				same = false;
				detail = static_cast<uint8_t>(i < 255u ? i : 255u);
				break;
			}
		}
	}
	record(same ? Status::Ok : Status::Mismatch, same ? 0u : detail, true, rtt);
	advance(HAL_GetTick());
}

} // namespace client
#endif

[[nodiscard]] bool send_role_report(const uint32_t token, const uint32_t page) noexcept
{
	Writer writer;
	bool ok = begin_response(writer, Command::RoleReport, token) &&
		writer.put_u8(static_cast<uint8_t>(MODBUS_HW_ROLE));
#if MODBUS_HW_ROLE == 2
	const std::size_t first = page * kReportEntries;
	const std::size_t in_page = first < client::kSteps
		? (client::kSteps - first < kReportEntries ? client::kSteps - first : kReportEntries) : 0u;
	ok = ok && writer.put_u8(client::s_engine.running ? 1u : 0u) &&
		writer.put_u8(client::s_engine.done ? 1u : 0u) &&
		writer.put_u8(static_cast<uint8_t>(client::kSteps)) &&
		writer.put_u8(static_cast<uint8_t>(page)) &&
		writer.put_u8(static_cast<uint8_t>(in_page)) &&
		writer.put_u8(static_cast<uint8_t>(MODBUS_HW_FRAMER)) &&
		writer.put_u8(0u);
#else
	ok = ok && writer.put_u8(0u) && writer.put_u8(0u) && writer.put_u8(0u) &&
		writer.put_u8(static_cast<uint8_t>(page)) && writer.put_u8(0u) &&
		writer.put_u8(static_cast<uint8_t>(MODBUS_HW_FRAMER)) && writer.put_u8(0u);
#endif
	ok = ok && writer.put_u32(s_role.served) && writer.put_u32(s_role.exceptions) &&
		writer.put_u32(s_role.broadcasts) && writer.put_u32(s_role.ignored);
	for (std::size_t i = 0u; i < kReportEntries && ok; ++i) {
#if MODBUS_HW_ROLE == 2
		const std::size_t index = first + i;
		client::Result result;
		if (index < client::kSteps) {
			result = client::s_engine.results[index];
		}
		ok = writer.put_u8(static_cast<uint8_t>(result.status)) && writer.put_u8(result.detail) &&
			writer.put_u8(result.responded) && writer.put_u8(result.reserved) &&
			writer.put_u32(result.rtt_us);
#else
		ok = writer.put_u32(0u) && writer.put_u32(0u);
#endif
	}
	return ok && send_writer(writer);
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
		writer.put_u32(static_cast<uint32_t>(MODBUS_HW_FRAMER)) &&
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
	const Link::Storage::Stats rx_pool = s_link.storage().rx_stats();
	const Link::Storage::Stats tx_pool = s_link.storage().tx_stats();
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

// The application body of a received packet: function data without the
// framing policy's length prefix (which the policy has already verified
// against the frame length).
[[nodiscard]] std::span<const uint8_t> body_of(const Link::Packet& packet) noexcept
{
	return packet.data().subspan(prefix_for(packet.function()));
}

[[nodiscard]] bool has_control_magic(const Link::Packet& packet) noexcept
{
	const std::span<const uint8_t> data = body_of(packet);
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
	case Command::RoleReport:
		(void)send_role_report(token, data.size() >= 13u ? argument : 0u);
		return;
	case Command::ResetModel:
		// Between two clients that each expect the initial contents, and
		// before a client script; the role counters stay.
		s_model.reset();
		(void)send_ack(command, token, 0u, 0u);
		return;
	case Command::StartClient:
#if MODBUS_HW_ROLE == 2
		if (data.size() < 13u || argument > kMaxActionMs) {
			(void)send_ack(command, token, 1u, argument);
			return;
		}
		if (send_ack(command, token, 0u, argument)) {
			client::start(HAL_GetTick(), argument);
		}
#else
		(void)send_ack(command, token, 3u, 0u);   // not a client image
#endif
		return;
	}
	(void)send_ack(command, token, 2u, 0u);
}

#if MODBUS_HW_ROLE == 1
// The server role: the reference model at unit 0x11, broadcasts executed
// silently, frames for other units ignored.
void serve_packet(const Link::Packet& packet) noexcept
{
	const modbus_reference::Reply reply = modbus_reference::serve(
		s_model, modbus_reference::kBoardUnit, packet.address(), packet.function(), body_of(packet));
	if (!reply.respond) {
		if (packet.address() == 0u) {
			++s_role.broadcasts;
		}
		return;
	}
	if (send_data(modbus_reference::kBoardUnit, reply.function, reply.span())) {
		++s_role.served;
		if (reply.exception()) {
			++s_role.exceptions;
		}
	}
}
#endif

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
				process_control(body_of(packet));
#if MODBUS_HW_ROLE == 1
			} else if (packet.address() == modbus_reference::kBoardUnit || packet.address() == 0u) {
				serve_packet(packet);
			} else {
				++s_role.ignored;
#elif MODBUS_HW_ROLE == 2
			} else {
				client::on_packet(packet);
#else
			} else if (send_data(packet.address(), packet.function(),
			                          body_of(packet))) {
				++s_app.echo_frames;
				s_app.echo_data_bytes +=
					static_cast<uint32_t>(body_of(packet).size());
#endif
			}
		}
	}
	bench_counter_add(&s_packet_process, DWT->CYCCNT - started);
}

void poll_link(const uint32_t now) noexcept
{
	// Only a call that actually released a block is charged to the release
	// counter: the peer requires exactly one such call per released frame, and
	// deciding from a separate tx_busy() read would race with the transmission
	// completing between that read and poll()'s own.
	const bool was_active = s_link.tx_active();
	const uint32_t started = DWT->CYCCNT;
	s_link.poll(now);
	if (was_active && !s_link.tx_active()) {
		bench_counter_add(&s_rtu_tx_release, DWT->CYCCNT - started);
	}
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

// The production integration object: RX/gap routing, the endpoint's transport
// binding and, with the framer, the stale-frame rule. The harness lets it
// bind everything, then re-points the RX handler at a wrapper that measures
// the adapter's on_rx() (consume()/receive_adu() plus the deadline
// arithmetic) — that is the rtu_receive counter.
using Adapter = modbus::rtu::UartAdapter<Serial, Link>;
Adapter s_adapter{s_uart, s_link}; // the line rate is read from huart3 at bind()
#if MODBUS_HW_WAKE
volatile uint32_t s_wakes = 0u;
#endif

void on_rx(const std::span<const uint8_t> bytes) noexcept
{
	const uint32_t started = DWT->CYCCNT;
	s_adapter.on_rx(bytes);
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

#if MODBUS_HW_WAKE
	s_uart.setWakeHandler([]() noexcept { s_wakes = s_wakes + 1u; }); // volatile read-modify-write; ++ on a volatile is deprecated in C++20
#endif
	// init() first: bind() reads the line rate from the driver's bound handle.
	if (!s_uart.init(&huart3) || !s_adapter.bind()) {
		Error_Handler();
	}
	// After bind(): the measuring wrapper replaces the adapter's direct RX
	// binding; the gap handler and the transport binding stay the adapter's.
	s_uart.setRxHandler([](const std::span<const uint8_t> bytes) noexcept {
		on_rx(bytes);
	});
	s_model.reset();
	reset_metrics();
}

extern "C" void bench_loop(void)
{
	// The adapter's proceed() is these four calls in this order; the harness
	// composes them itself to keep its DWT scopes around the driver and the
	// release. finish() after the driver: a continuation already queued must
	// be delivered before its frame can be judged stale.
	const uint32_t now = HAL_GetTick();
	s_adapter.prepare(now);
	s_uart.proceed(now);
	s_adapter.finish(now);
	poll_link(now);
	apply_pending_action(now);

	if (s_hold_active && deadline_pending(now, s_hold_until)) {
		return; // receive into the Endpoint while retaining every ready Packet
	}
	if (s_hold_active) {
		s_hold_active = false;
	}
	process_one_packet();
#if MODBUS_HW_ROLE == 2
	client::step(now);
#endif
}
