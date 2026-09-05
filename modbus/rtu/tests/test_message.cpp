/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"
#include "Test.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

using namespace modbus_test;

namespace {

class Capture final {
public:
	bool accept = true;
	bool borrowed = false;
	std::vector<uint8_t> last{};
	std::vector<std::vector<uint8_t>> attempts{};

	[[nodiscard]] bool send(const std::span<const uint8_t> adu) noexcept
	{
		attempts.emplace_back(adu.begin(), adu.end());
		if (!accept || borrowed) {
			return false;
		}
		last.assign(adu.begin(), adu.end());
		borrowed = true;
		return true;
	}

	[[nodiscard]] bool busy() const noexcept { return borrowed; }
	void finish() noexcept { borrowed = false; }
};

template<class Endpoint>
bool bind(Endpoint& endpoint, Capture& capture)
{
	return endpoint.bind(
		typename Endpoint::Sender{tiny::bind<&Capture::send>(capture)},
		typename Endpoint::BusyQuery{tiny::bind<&Capture::busy>(capture)});
}

enum class WireCode : uint16_t { Value = 0xA1B2u };

struct Record {
	uint8_t byte;
	uint32_t word;
};

template<class M, class T>
concept CanAppendNative = requires(M& message, const T& value) {
	message.append_native(value);
};

template<class M, class T>
concept CanAppendBe = requires(M& message, const T& value) {
	message.append_be(value);
};

template<class M, class T>
concept CanAppendLe = requires(M& message, const T& value) {
	message.append_le(value);
};

template<class M, class T>
concept CanAppendBeSpan = requires(M& message, std::span<const T> values) {
	message.append_be(values);
};

template<class M>
concept HasAppendU8 = requires(M& message) { message.append_u8(uint8_t{}); };

template<class M>
concept HasAppendBe16 = requires(M& message) { message.append_be16(uint16_t{}); };

} // namespace

int main()
{
	using Endpoint = modbus::rtu::Endpoint<wire::Pool<2, 2>>;
	using Message = Endpoint::Message;
	static_assert(!std::is_copy_constructible_v<Message>);
	static_assert(std::is_nothrow_move_constructible_v<Message>);

	Endpoint endpoint;
	Capture transport;
	check(bind(endpoint, transport), "transport binds as one sender/busy pair");

	group("Builder");
	auto message = endpoint.make_message(0x01u, 0x03u, 5u);
	check(message && message.size() == 0u && message.capacity() == 252u,
	      "Pool message starts empty and reports its full paid-for capacity");
	check(message.address() == 0x01u && message.function() == 0x03u,
	      "address/function are immutable message metadata");
	check(message.append_be(uint16_t{0x0010u}) &&
	      message.append_be(uint16_t{0x0002u}) &&
	      message.append_be(uint8_t{0xA5u}),
	      "generic big-endian serializers append 16- and 8-bit function data");
	check(message.size() == 5u, "size counts only appended function data");
	check(endpoint.send(message) == modbus::SendResult::Sent && !message,
	      "Sent moves exclusive ownership into Endpoint");
	const std::array<uint8_t, 7> expected_prefix{
		0x01u, 0x03u, 0x00u, 0x10u, 0x00u, 0x02u, 0xA5u};
	check(transport.last.size() == 9u &&
	      std::equal(expected_prefix.begin(), expected_prefix.end(), transport.last.begin()),
	      "wire layout is address, function, big-endian data");
	check(::crc::verify<::crc::Crc16Bitwise>(transport.last),
	      "Message appends a valid low-byte-first CRC automatically");
	check(endpoint.storage().tx_available() == 1u && endpoint.tx_active(),
	      "Endpoint owns the pool block while transport borrows it");
	endpoint.poll();
	check(endpoint.storage().tx_available() == 1u,
	      "poll does not reclaim memory while busy() is true");
	transport.finish();
	endpoint.poll();
	check(endpoint.storage().tx_available() == 2u && !endpoint.tx_active(),
	      "poll reclaims memory only after transport releases the borrow");

	group("UniversalSerializers");
	const uint16_t native16 = 0x1122u;
	constexpr uint32_t be32 = 0x01020304u;
	constexpr uint32_t le32 = 0xA1B2C3D4u;
	constexpr float be_float = 1.5F;
	static_assert(sizeof(be_float) == 4u && std::numeric_limits<float>::is_iec559);
	constexpr std::array<uint16_t, 2> be_words{0x1020u, 0x3040u};
	constexpr std::array<uint16_t, 2> le_words{0x5060u, 0x7080u};
	auto universal = endpoint.make_message(0x22u, 0x41u, 24u);
	check(universal.append_native(native16) &&
	      universal.append_be(be32) && universal.append_le(le32) &&
	      universal.append_be(WireCode::Value) && universal.append_be(be_float) &&
	      universal.append_be(std::span<const uint16_t>{be_words}) &&
	      universal.append_le(std::span<const uint16_t>{le_words}),
	      "native, big-endian, little-endian and ordered spans share one API");
	std::vector<uint8_t> universal_prefix{0x22u, 0x41u};
	const auto* const native_bytes = reinterpret_cast<const uint8_t*>(&native16);
	universal_prefix.insert(universal_prefix.end(), native_bytes,
	                        native_bytes + sizeof(native16));
	const std::array<uint8_t, 22> ordered{
		0x01u, 0x02u, 0x03u, 0x04u,
		0xD4u, 0xC3u, 0xB2u, 0xA1u,
		0xA1u, 0xB2u,
		0x3Fu, 0xC0u, 0x00u, 0x00u,
		0x10u, 0x20u, 0x30u, 0x40u,
		0x60u, 0x50u, 0x80u, 0x70u};
	universal_prefix.insert(universal_prefix.end(), ordered.begin(), ordered.end());
	check(endpoint.send(universal) == modbus::SendResult::Sent &&
	      transport.last.size() == universal_prefix.size() + 2u &&
	      std::equal(universal_prefix.begin(), universal_prefix.end(),
	                 transport.last.begin()) &&
	      ::crc::verify<::crc::Crc16Bitwise>(transport.last),
	      "every serializer produces exact bytes before the library-owned CRC");
	transport.finish();
	endpoint.poll();

	group("SerializerTypeContract");
	static_assert(CanAppendNative<Message, uint32_t> &&
	              CanAppendBe<Message, uint32_t> && CanAppendLe<Message, uint32_t>);
	static_assert(CanAppendNative<Message, float> &&
	              CanAppendBe<Message, float> && CanAppendLe<Message, float>);
	static_assert(CanAppendNative<Message, WireCode> &&
	              CanAppendBe<Message, WireCode> && CanAppendLe<Message, WireCode>);
	static_assert(!CanAppendNative<Message, bool> && !CanAppendBe<Message, bool> &&
	              !CanAppendLe<Message, bool>);
	static_assert(!CanAppendNative<Message, Record> && !CanAppendBe<Message, Record> &&
	              !CanAppendLe<Message, Record>);
	static_assert(CanAppendNative<Message, long double> &&
	              !CanAppendBe<Message, long double> &&
	              !CanAppendLe<Message, long double>);
	static_assert(CanAppendBeSpan<Message, uint16_t> &&
	              !CanAppendBeSpan<Message, Record>);
	static_assert(!HasAppendU8<Message> && !HasAppendBe16<Message>);
	check(true, "one constrained scalar contract replaces width-specific methods");

	group("BusyKeepsBuilding");
	transport.borrowed = true;
	auto busy = endpoint.make_message(2u, 0x64u, 1u);
	check(busy.append_native(uint8_t{0x11u}), "busy-case message starts Building");
	check(endpoint.send(busy) == modbus::SendResult::Busy && busy,
	      "Busy retains caller ownership");
	check(busy.append_native(uint8_t{0x22u}) && busy.size() == 2u,
	      "Busy does not finalize and the caller may continue building");
	transport.borrowed = false;
	check(endpoint.send(busy) == modbus::SendResult::Sent,
	      "the same message sends after backpressure clears");
	transport.finish();
	endpoint.poll();

	group("FailedRetryIdentity");
	transport.accept = false;
	auto retry = endpoint.make_message(7u, 0xA7u, 3u);
	const std::array<uint8_t, 3> raw{0x00u, 0xFEu, 0x55u};
	check(retry.append_bytes(raw), "retry message accepts arbitrary custom data");
	check(endpoint.send(retry) == modbus::SendResult::Failed && retry,
	      "a refused transport start keeps Message ownership");
	check(!retry.append_native(uint8_t{0x99u}),
	      "a finalized retry is immutable rather than silently changing wire bytes");
	const auto first_attempt = transport.attempts.back();
	transport.accept = true;
	check(endpoint.send(retry) == modbus::SendResult::Sent,
	      "the finalized Message can retry");
	check(transport.attempts.back() == first_attempt,
	      "retry submits the byte-identical ADU including CRC");
	transport.finish();
	endpoint.poll();

	group("GrowthAndStrongFailure");
	modbus::rtu::Endpoint<> heap;
	Capture heap_transport;
	check(bind(heap, heap_transport), "heap endpoint binds");
	auto growing = heap.make_message(3u, 0x10u, 0u);
	check(growing && growing.capacity() == 0u,
	      "zero hint reserves no function-data capacity");
	for (uint8_t i = 0u; i < 20u; ++i) {
		check(growing.append_native(i), "heap message grows geometrically on demand");
	}
	const std::size_t size_before = growing.size();
	const std::size_t capacity_before = growing.capacity();
	std::vector<uint8_t> too_much(modbus::max_data_size, 0xEEu);
	check(!growing.append_bytes(too_much) && growing.size() == size_before &&
	      growing.capacity() == capacity_before,
	      "failed oversize append leaves size and capacity unchanged");
	const std::array<uint64_t, 32> too_many_words{};
	check(!growing.append_be(std::span<const uint64_t>{too_many_words}) &&
	      growing.size() == size_before && growing.capacity() == capacity_before,
	      "ordered-span overflow is rejected before multiplication or growth");
	check(heap.send(growing) == modbus::SendResult::Sent &&
	      heap_transport.last.size() == size_before + 4u,
	      "the intact message still sends after a failed append");
	heap_transport.finish();
	heap.poll();

	group("MaximumAndMove");
	auto maximum = endpoint.make_message(0xF7u, 0x2Bu, modbus::max_data_size);
	std::vector<uint8_t> maximum_data(modbus::max_data_size, 0x5Au);
	check(maximum.append_bytes(maximum_data), "all 252 function-data bytes fit");
	check(!maximum.append_le(uint8_t{0xFFu}) && maximum.size() == modbus::max_data_size,
	      "an ordered append past the exact limit changes nothing");
	Message moved = std::move(maximum);
	check(!maximum && moved && moved.size() == 252u,
	      "move transfers exclusive ownership and logical size");
	check(endpoint.send(moved) == modbus::SendResult::Sent &&
	      transport.last.size() == 256u && ::crc::verify<::crc::Crc16Bitwise>(transport.last),
	      "maximum Message becomes an exact valid 256-byte RTU ADU");
	transport.finish();
	endpoint.poll();
	check(!endpoint.make_message(1u, 3u, modbus::max_data_size + 1u),
	      "capacity hint above the RTU limit yields an empty Message");

	return finish();
}
