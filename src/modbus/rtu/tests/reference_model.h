/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * A reference Modbus server data model, used as the same thing by three
 * parties so their results can be compared byte for byte:
 *
 *   - the H7S harness firmware in its server role serves it to a client on
 *     the PC (Qt's QModbusRtuSerialClient, or this repository's RtuClient);
 *   - the same firmware in its client role keeps it as a shadow of the PC's
 *     QModbusRtuSerialServer, predicts every response from it, and reports
 *     where the real one differed;
 *   - the Qt runner on the PC initialises Qt's server map from it and uses it
 *     as the oracle for the responses the board's server sends.
 *
 * It is a test fixture, not a library component: the standard functions a
 * PLC-style device answers (01–06, 08/00, 0F, 10, 17), the three exception
 * codes the specification defines for them, broadcast writes, and nothing
 * else. Tables are sized for the 256-register / 512-bit map the runner gives
 * Qt's server. Header-only, freestanding: it compiles on Cortex-M and on the
 * desktop alike.
 */

#ifndef MODBUS_RTU_TESTS_REFERENCE_MODEL_H_
#define MODBUS_RTU_TESTS_REFERENCE_MODEL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace modbus_reference {

constexpr uint8_t kBoardUnit = 0x11u;   // the board as a server
constexpr uint8_t kPcUnit = 0x0Au;      // the PC's QModbus server
constexpr uint8_t kForeignUnit = 0x22u; // nobody: requests to it must time out

constexpr std::size_t kHoldingCount = 256u;
constexpr std::size_t kInputCount = 256u;
constexpr std::size_t kCoilCount = 512u;
constexpr std::size_t kDiscreteCount = 512u;

// The response body limit of a 256-byte RTU ADU: 256 - address - function - CRC.
constexpr std::size_t kMaxData = 252u;

enum class Exception : uint8_t {
	None = 0u,
	IllegalFunction = 0x01u,
	IllegalDataAddress = 0x02u,
	IllegalDataValue = 0x03u,
};

struct Model final {
	std::array<uint16_t, kHoldingCount> holding{};
	std::array<uint16_t, kInputCount> input{};
	std::array<uint8_t, kCoilCount / 8u> coils{};
	std::array<uint8_t, kDiscreteCount / 8u> discrete{};

	// The initial contents every party starts from.
	void reset() noexcept
	{
		for (std::size_t i = 0u; i < kHoldingCount; ++i) {
			holding[i] = static_cast<uint16_t>(0x1000u + i);
		}
		for (std::size_t i = 0u; i < kInputCount; ++i) {
			input[i] = static_cast<uint16_t>(0x2000u + i);
		}
		coils.fill(0u);
		discrete.fill(0u);
		for (std::size_t i = 0u; i < kCoilCount; ++i) {
			set_bit(coils, i, i % 3u == 0u);
		}
		for (std::size_t i = 0u; i < kDiscreteCount; ++i) {
			set_bit(discrete, i, i % 2u == 0u);
		}
	}

	template<std::size_t N>
	[[nodiscard]] static bool bit(const std::array<uint8_t, N>& bits, const std::size_t index) noexcept
	{
		return ((static_cast<unsigned>(bits[index / 8u]) >> (index % 8u)) & 1u) != 0u;
	}

	template<std::size_t N>
	static void set_bit(std::array<uint8_t, N>& bits, const std::size_t index, const bool value) noexcept
	{
		const uint8_t mask = static_cast<uint8_t>(1u << (index % 8u));
		if (value) {
			bits[index / 8u] = static_cast<uint8_t>(bits[index / 8u] | mask);
		} else {
			bits[index / 8u] = static_cast<uint8_t>(bits[index / 8u] & static_cast<uint8_t>(~mask));
		}
	}
};

struct Reply final {
	bool respond = false;              // false: broadcast, or a request for another unit
	uint8_t function = 0u;             // the request's function, or function | 0x80
	std::array<uint8_t, kMaxData> data{};
	std::size_t size = 0u;

	[[nodiscard]] std::span<const uint8_t> span() const noexcept { return {data.data(), size}; }
	[[nodiscard]] bool exception() const noexcept { return (function & 0x80u) != 0u; }
	[[nodiscard]] Exception code() const noexcept
	{
		return exception() && size == 1u ? static_cast<Exception>(data[0]) : Exception::None;
	}

	[[nodiscard]] bool put(const uint8_t value) noexcept
	{
		if (size >= data.size()) {
			return false;
		}
		data[size++] = value;
		return true;
	}

	[[nodiscard]] bool put_be16(const uint16_t value) noexcept
	{
		return put(static_cast<uint8_t>(value >> 8u)) && put(static_cast<uint8_t>(value));
	}
};

namespace detail {

[[nodiscard]] inline uint16_t be16(const std::span<const uint8_t> data, const std::size_t offset) noexcept
{
	return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8u) | data[offset + 1u]);
}

[[nodiscard]] inline Reply exception_reply(const uint8_t function, const Exception code) noexcept
{
	Reply reply;
	reply.respond = true;
	reply.function = static_cast<uint8_t>(function | 0x80u);
	(void)reply.put(static_cast<uint8_t>(code));
	return reply;
}

template<std::size_t N>
[[nodiscard]] inline Reply read_bits(
		const std::array<uint8_t, N>& bits,
		const std::size_t count,
		const uint8_t function,
		const std::span<const uint8_t> data) noexcept
{
	if (data.size() != 4u) {
		return exception_reply(function, Exception::IllegalDataValue);
	}
	const uint16_t start = be16(data, 0u);
	const uint16_t quantity = be16(data, 2u);
	if (quantity < 1u || quantity > 2000u) {
		return exception_reply(function, Exception::IllegalDataValue);
	}
	if (static_cast<std::size_t>(start) + quantity > count) {
		return exception_reply(function, Exception::IllegalDataAddress);
	}
	Reply reply;
	reply.respond = true;
	reply.function = function;
	const std::size_t bytes = (quantity + 7u) / 8u;
	(void)reply.put(static_cast<uint8_t>(bytes));
	for (std::size_t b = 0u; b < bytes; ++b) {
		uint8_t packed = 0u;
		for (std::size_t bit = 0u; bit < 8u; ++bit) {
			const std::size_t index = start + b * 8u + bit;
			if (index < static_cast<std::size_t>(start) + quantity && Model::bit(bits, index)) {
				packed = static_cast<uint8_t>(packed | (1u << bit));
			}
		}
		(void)reply.put(packed);
	}
	return reply;
}

template<std::size_t N>
[[nodiscard]] inline Reply read_registers(
		const std::array<uint16_t, N>& registers,
		const uint8_t function,
		const std::span<const uint8_t> data) noexcept
{
	if (data.size() != 4u) {
		return exception_reply(function, Exception::IllegalDataValue);
	}
	const uint16_t start = be16(data, 0u);
	const uint16_t quantity = be16(data, 2u);
	if (quantity < 1u || quantity > 125u) {
		return exception_reply(function, Exception::IllegalDataValue);
	}
	if (static_cast<std::size_t>(start) + quantity > N) {
		return exception_reply(function, Exception::IllegalDataAddress);
	}
	Reply reply;
	reply.respond = true;
	reply.function = function;
	(void)reply.put(static_cast<uint8_t>(quantity * 2u));
	for (std::size_t i = 0u; i < quantity; ++i) {
		(void)reply.put_be16(registers[start + i]);
	}
	return reply;
}

} // namespace detail

/*
 * Serves one request the way a server with unit id `unit` does. Writes are
 * applied to `model` for the unit and for broadcasts (address 0), which are
 * not answered; a request for any other address is ignored and leaves the
 * model alone. The reply carries the function (with the exception bit when
 * the request was refused) and the data bytes that follow it on the wire.
 */
[[nodiscard]] inline Reply serve(
		Model& model,
		const uint8_t unit,
		const uint8_t address,
		const uint8_t function,
		const std::span<const uint8_t> data) noexcept
{
	using detail::be16;
	using detail::exception_reply;

	Reply reply;
	if (address != unit && address != 0u) {
		return reply;   // not for us
	}
	const bool broadcast = address == 0u;

	switch (function) {
	case 0x01u:
		reply = detail::read_bits(model.coils, kCoilCount, function, data);
		break;
	case 0x02u:
		reply = detail::read_bits(model.discrete, kDiscreteCount, function, data);
		break;
	case 0x03u:
		reply = detail::read_registers(model.holding, function, data);
		break;
	case 0x04u:
		reply = detail::read_registers(model.input, function, data);
		break;
	case 0x05u: {   // write single coil
		if (data.size() != 4u) {
			reply = exception_reply(function, Exception::IllegalDataValue);
			break;
		}
		const uint16_t output = be16(data, 0u);
		const uint16_t value = be16(data, 2u);
		if (value != 0x0000u && value != 0xFF00u) {
			reply = exception_reply(function, Exception::IllegalDataValue);
			break;
		}
		if (output >= kCoilCount) {
			reply = exception_reply(function, Exception::IllegalDataAddress);
			break;
		}
		Model::set_bit(model.coils, output, value == 0xFF00u);
		reply.respond = true;
		reply.function = function;
		for (const uint8_t byte : data) {
			(void)reply.put(byte);   // the response echoes the request
		}
		break;
	}
	case 0x06u: {   // write single register
		if (data.size() != 4u) {
			reply = exception_reply(function, Exception::IllegalDataValue);
			break;
		}
		const uint16_t address16 = be16(data, 0u);
		if (address16 >= kHoldingCount) {
			reply = exception_reply(function, Exception::IllegalDataAddress);
			break;
		}
		model.holding[address16] = be16(data, 2u);
		reply.respond = true;
		reply.function = function;
		for (const uint8_t byte : data) {
			(void)reply.put(byte);
		}
		break;
	}
	case 0x08u: {   // diagnostics: only sub-function 0, return query data
		if (data.size() < 2u) {
			reply = exception_reply(function, Exception::IllegalDataValue);
			break;
		}
		if (be16(data, 0u) != 0x0000u) {
			reply = exception_reply(function, Exception::IllegalFunction);
			break;
		}
		reply.respond = true;
		reply.function = function;
		for (const uint8_t byte : data) {
			(void)reply.put(byte);
		}
		break;
	}
	case 0x0Fu: {   // write multiple coils
		if (data.size() < 5u) {
			reply = exception_reply(function, Exception::IllegalDataValue);
			break;
		}
		const uint16_t start = be16(data, 0u);
		const uint16_t quantity = be16(data, 2u);
		const uint8_t byte_count = data[4];
		if (quantity < 1u || quantity > 1968u || byte_count != (quantity + 7u) / 8u ||
		    data.size() != 5u + static_cast<std::size_t>(byte_count)) {
			reply = exception_reply(function, Exception::IllegalDataValue);
			break;
		}
		if (static_cast<std::size_t>(start) + quantity > kCoilCount) {
			reply = exception_reply(function, Exception::IllegalDataAddress);
			break;
		}
		for (std::size_t i = 0u; i < quantity; ++i) {
			const bool value = ((static_cast<unsigned>(data[5u + i / 8u]) >> (i % 8u)) & 1u) != 0u;
			Model::set_bit(model.coils, start + i, value);
		}
		reply.respond = true;
		reply.function = function;
		(void)reply.put_be16(start);
		(void)reply.put_be16(quantity);
		break;
	}
	case 0x10u: {   // write multiple registers
		if (data.size() < 5u) {
			reply = exception_reply(function, Exception::IllegalDataValue);
			break;
		}
		const uint16_t start = be16(data, 0u);
		const uint16_t quantity = be16(data, 2u);
		const uint8_t byte_count = data[4];
		if (quantity < 1u || quantity > 123u || byte_count != quantity * 2u ||
		    data.size() != 5u + static_cast<std::size_t>(byte_count)) {
			reply = exception_reply(function, Exception::IllegalDataValue);
			break;
		}
		if (static_cast<std::size_t>(start) + quantity > kHoldingCount) {
			reply = exception_reply(function, Exception::IllegalDataAddress);
			break;
		}
		for (std::size_t i = 0u; i < quantity; ++i) {
			model.holding[start + i] = be16(data, 5u + i * 2u);
		}
		reply.respond = true;
		reply.function = function;
		(void)reply.put_be16(start);
		(void)reply.put_be16(quantity);
		break;
	}
	case 0x17u: {   // read/write multiple registers: write first, then read
		if (data.size() < 9u) {
			reply = exception_reply(function, Exception::IllegalDataValue);
			break;
		}
		const uint16_t read_start = be16(data, 0u);
		const uint16_t read_quantity = be16(data, 2u);
		const uint16_t write_start = be16(data, 4u);
		const uint16_t write_quantity = be16(data, 6u);
		const uint8_t byte_count = data[8];
		if (read_quantity < 1u || read_quantity > 125u || write_quantity < 1u ||
		    write_quantity > 121u || byte_count != write_quantity * 2u ||
		    data.size() != 9u + static_cast<std::size_t>(byte_count)) {
			reply = exception_reply(function, Exception::IllegalDataValue);
			break;
		}
		if (static_cast<std::size_t>(read_start) + read_quantity > kHoldingCount ||
		    static_cast<std::size_t>(write_start) + write_quantity > kHoldingCount) {
			reply = exception_reply(function, Exception::IllegalDataAddress);
			break;
		}
		for (std::size_t i = 0u; i < write_quantity; ++i) {
			model.holding[write_start + i] = be16(data, 9u + i * 2u);
		}
		reply.respond = true;
		reply.function = function;
		(void)reply.put(static_cast<uint8_t>(read_quantity * 2u));
		for (std::size_t i = 0u; i < read_quantity; ++i) {
			(void)reply.put_be16(model.holding[read_start + i]);
		}
		break;
	}
	default:
		reply = exception_reply(function, Exception::IllegalFunction);
		break;
	}

	if (broadcast) {
		reply.respond = false;   // executed (writes), never answered
	}
	return reply;
}

/*
 * The request script both comparisons run, byte for byte the same whether the
 * board's client sends it to Qt's server or a PC client sends it to the
 * board's server: reads, writes each verified by a read-back, the largest
 * legal frames, the exceptions, a diagnostics echo, a request nobody answers,
 * a broadcast verified by a read-back, and a burst of identical reads for
 * timing. `unit` is the server being addressed; two steps override it.
 */
namespace script {

constexpr std::size_t kScriptedSteps = 25u;
constexpr std::size_t kBackToBackReads = 30u;
constexpr std::size_t kSteps = kScriptedSteps + kBackToBackReads;
constexpr std::size_t kUnknownFunctionStep = 19u;   // a framed client refuses to send it
constexpr std::size_t kForeignUnitStep = 22u;       // nobody answers it
constexpr std::size_t kBroadcastStep = 23u;         // executed, never answered

struct Request final {
	uint8_t address = 0u;
	uint8_t function = 0u;
	std::size_t size = 0u;
	std::array<uint8_t, kMaxData> data{};

	void put(const uint8_t value) noexcept { data[size++] = value; }
	void put_be16(const uint16_t value) noexcept
	{
		put(static_cast<uint8_t>(value >> 8u));
		put(static_cast<uint8_t>(value));
	}
	[[nodiscard]] std::span<const uint8_t> span() const noexcept { return {data.data(), size}; }
};

inline void build(const std::size_t index, const uint8_t unit, Request& request) noexcept
{
	request = Request{};
	request.address = unit;
	const auto read = [&request](const uint8_t function, const uint16_t start, const uint16_t quantity) {
		request.function = function;
		request.put_be16(start);
		request.put_be16(quantity);
	};
	switch (index) {
	case 0u: read(0x03u, 0u, 10u); break;
	case 1u: read(0x04u, 0u, 10u); break;
	case 2u: read(0x01u, 0u, 16u); break;
	case 3u: read(0x02u, 0u, 16u); break;
	case 4u: request.function = 0x06u; request.put_be16(5u); request.put_be16(0xBEEFu); break;
	case 5u: read(0x03u, 0u, 10u); break;
	case 6u: request.function = 0x05u; request.put_be16(7u); request.put_be16(0xFF00u); break;
	case 7u: read(0x01u, 0u, 16u); break;
	case 8u:
		request.function = 0x10u; request.put_be16(10u); request.put_be16(10u); request.put(20u);
		for (uint16_t i = 0u; i < 10u; ++i) { request.put_be16(static_cast<uint16_t>(0xA000u + i)); }
		break;
	case 9u: read(0x03u, 10u, 10u); break;
	case 10u:
		request.function = 0x0Fu; request.put_be16(16u); request.put_be16(24u); request.put(3u);
		request.put(0xCDu); request.put(0x6Bu); request.put(0xB2u);
		break;
	case 11u: read(0x01u, 16u, 24u); break;
	case 12u:
		request.function = 0x17u; request.put_be16(0u); request.put_be16(5u);
		request.put_be16(100u); request.put_be16(4u); request.put(8u);
		for (uint16_t i = 0u; i < 4u; ++i) { request.put_be16(static_cast<uint16_t>(0x5500u + i)); }
		break;
	case 13u: read(0x03u, 100u, 4u); break;
	case 14u: read(0x03u, 0u, 125u); break;
	case 15u:
		request.function = 0x10u; request.put_be16(20u); request.put_be16(123u); request.put(246u);
		for (uint16_t i = 0u; i < 123u; ++i) { request.put_be16(static_cast<uint16_t>(0x7000u + i)); }
		break;
	case 16u: read(0x03u, 20u, 123u); break;
	case 17u: read(0x03u, 300u, 5u); break;                       // illegal data address
	case 18u: read(0x03u, 0u, 0u); break;                         // illegal data value
	case kUnknownFunctionStep: request.function = 0x64u; break;   // illegal function
	case 20u: request.function = 0x08u; request.put_be16(0u); request.put(0xA5u); request.put(0x37u); break;
	case 21u: request.function = 0x05u; request.put_be16(1u); request.put_be16(0x1234u); break;   // illegal data value
	case kForeignUnitStep:
		request.address = kForeignUnit; request.function = 0x06u; request.put_be16(0u); request.put_be16(1u);
		break;
	case kBroadcastStep:
		request.address = 0u; request.function = 0x06u; request.put_be16(6u); request.put_be16(0x1234u);
		break;
	case 24u: read(0x03u, 6u, 1u); break;
	default: read(0x03u, 0u, 10u); break;                         // the back-to-back burst
	}
}

inline const char* name(const std::size_t index) noexcept
{
	switch (index) {
	case 0u: return "read holding 0..9";
	case 1u: return "read input 0..9";
	case 2u: return "read coils 0..15";
	case 3u: return "read discrete 0..15";
	case 4u: return "write register 5";
	case 5u: return "read back holding 0..9";
	case 6u: return "write coil 7 on";
	case 7u: return "read back coils 0..15";
	case 8u: return "write 10 registers at 10";
	case 9u: return "read back 10..19";
	case 10u: return "write 24 coils at 16";
	case 11u: return "read back coils 16..39";
	case 12u: return "read/write multiple";
	case 13u: return "read back 100..103";
	case 14u: return "read 125 registers";
	case 15u: return "write 123 registers";
	case 16u: return "read back 123 registers";
	case 17u: return "exception: address out of range";
	case 18u: return "exception: quantity 0";
	case kUnknownFunctionStep: return "exception: unknown function 0x64";
	case 20u: return "diagnostics echo";
	case 21u: return "exception: bad coil value";
	case kForeignUnitStep: return "foreign unit: no answer";
	case kBroadcastStep: return "broadcast write register 6";
	case 24u: return "read back register 6";
	default: return "back-to-back read";
	}
}

} // namespace script

} // namespace modbus_reference

#endif /* MODBUS_RTU_TESTS_REFERENCE_MODEL_H_ */
