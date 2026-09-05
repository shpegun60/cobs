/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Compile-time guard for the deliberately shared COBS/Modbus RTU API shape. */

#include "cobs/Cobs.h"
#include "modbus/rtu/Rtu.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <type_traits>
#include <utility>

namespace {

using CobsEndpoint = cobs::Endpoint<wire::Pool<2u, 2u>>;
using CobsTableEndpoint = cobs::Endpoint<wire::Pool<2u, 2u>, cobs::Format<::crc::Crc16Table>>;
static_assert(std::same_as<CobsEndpoint::Layout, CobsTableEndpoint::Layout>);
static_assert(std::same_as<CobsEndpoint::Geometry, CobsTableEndpoint::Geometry>);
static_assert(std::same_as<CobsEndpoint::Storage, CobsTableEndpoint::Storage>);
static_assert(std::same_as<CobsEndpoint::Message, CobsTableEndpoint::Message>);
static_assert(std::same_as<CobsEndpoint::Packet, CobsTableEndpoint::Packet>);
static_assert(!std::same_as<CobsEndpoint::Format, CobsTableEndpoint::Format>);
static_assert(CobsEndpoint::max_send_size == 253u);
static_assert(std::same_as<CobsEndpoint::Crc, ::crc::Crc16Bitwise>);
static_assert(sizeof(CobsEndpoint) == sizeof(CobsTableEndpoint));
using RtuEndpoint = modbus::rtu::Endpoint<wire::Pool<2u, 2u>>;
using RtuTableEndpoint = modbus::rtu::Endpoint<wire::Pool<2u, 2u>, modbus::rtu::Format<::crc::Crc16Table>>;
using RtuNoCrcEndpoint = modbus::rtu::Endpoint<wire::Pool<2u, 2u>, modbus::rtu::Format<::crc::NoCrc>>;

template<class Packet>
concept SharedPacket =
	std::is_copy_constructible_v<Packet> &&
	std::is_copy_assignable_v<Packet> &&
	std::is_nothrow_move_constructible_v<Packet> &&
	std::is_nothrow_move_assignable_v<Packet> &&
	requires(Packet& packet, const Packet& view) {
		{ static_cast<bool>(view) } -> std::same_as<bool>;
		{ packet.reset() } -> std::same_as<void>;
		{ view.data() } -> std::same_as<std::span<const uint8_t>>;
		{ view.size() } -> std::same_as<std::size_t>;
	};

template<class Message>
concept SharedMessage =
	!std::is_copy_constructible_v<Message> &&
	!std::is_copy_assignable_v<Message> &&
	std::is_nothrow_move_constructible_v<Message> &&
	std::is_nothrow_move_assignable_v<Message> &&
	requires(Message& message,
	         const Message& view,
	         const uint16_t scalar,
	         const std::span<const uint16_t> scalars,
	         const std::span<const uint8_t> bytes,
	         const std::size_t required) {
		{ static_cast<bool>(view) } -> std::same_as<bool>;
		{ view.size() } -> std::same_as<std::size_t>;
		{ view.capacity() } -> std::same_as<std::size_t>;
		{ message.append_native(scalar) } -> std::same_as<bool>;
		{ message.append_be(scalar) } -> std::same_as<bool>;
		{ message.append_le(scalar) } -> std::same_as<bool>;
		{ message.append_native(scalars) } -> std::same_as<bool>;
		{ message.append_be(scalars) } -> std::same_as<bool>;
		{ message.append_le(scalars) } -> std::same_as<bool>;
		{ message.append_bytes(bytes) } -> std::same_as<bool>;
		{ message.reserve(required) } -> std::same_as<bool>;
	};

template<class Result>
concept SharedSendResult = std::is_enum_v<Result> && requires {
	Result::Sent;
	Result::Busy;
	Result::Unbound;
	Result::Failed;
	Result::Invalid;
};

template<class Endpoint>
concept SharedEndpoint =
	requires(Endpoint& endpoint,
	         typename Endpoint::Message& message,
	         typename Endpoint::Sender sender,
	         typename Endpoint::BusyQuery busy) {
		{ endpoint.bind(std::move(sender), std::move(busy)) } -> std::same_as<bool>;
		{ endpoint.unbind() } -> std::same_as<bool>;
		{ endpoint.notify_gap() } -> std::same_as<void>;
		{ endpoint.has_packet() } -> std::same_as<bool>;
		{ endpoint.pop_packet() } -> std::same_as<typename Endpoint::Packet>;
		endpoint.send(message);
		requires SharedSendResult<decltype(endpoint.send(message))>;
		{ endpoint.tx_active() } -> std::same_as<bool>;
		{ endpoint.poll() } -> std::same_as<void>;
		endpoint.stats();
		endpoint.storage();
	};

template<class Packet>
concept HasRtuMetadata = requires(const Packet& packet) {
	{ packet.address() } -> std::same_as<uint8_t>;
	{ packet.function() } -> std::same_as<uint8_t>;
	{ packet.pdu() } -> std::same_as<std::span<const uint8_t>>;
	{ packet.adu() } -> std::same_as<std::span<const uint8_t>>;
};

template<class Endpoint>
concept HasStreamConsume = requires(
		Endpoint& endpoint, const std::span<const uint8_t> bytes) {
	{ endpoint.consume(bytes) } -> std::same_as<void>;
};

template<class Endpoint>
concept HasAduReceive = requires(
		Endpoint& endpoint, const std::span<const uint8_t> bytes) {
	{ endpoint.receive_adu(bytes) } -> std::same_as<void>;
};

static_assert(SharedPacket<typename CobsEndpoint::Packet>);
static_assert(SharedPacket<typename RtuEndpoint::Packet>);
static_assert(SharedPacket<typename RtuTableEndpoint::Packet>);
static_assert(SharedPacket<typename RtuNoCrcEndpoint::Packet>);
static_assert(SharedMessage<typename CobsEndpoint::Message>);
static_assert(SharedMessage<typename RtuEndpoint::Message>);
static_assert(SharedMessage<typename RtuTableEndpoint::Message>);
static_assert(SharedMessage<typename RtuNoCrcEndpoint::Message>);
static_assert(SharedEndpoint<CobsEndpoint>);
static_assert(SharedEndpoint<CobsTableEndpoint>);
static_assert(SharedEndpoint<RtuEndpoint>);
static_assert(SharedEndpoint<RtuTableEndpoint>);
static_assert(SharedEndpoint<RtuNoCrcEndpoint>);
static_assert(std::same_as<RtuEndpoint::Message, RtuTableEndpoint::Message>);
static_assert(std::same_as<RtuEndpoint::Packet, RtuTableEndpoint::Packet>);
static_assert(std::same_as<RtuEndpoint::Layout, RtuTableEndpoint::Layout>);
static_assert(std::same_as<RtuEndpoint::Storage, RtuTableEndpoint::Storage>);
static_assert(std::same_as<RtuEndpoint::Geometry, RtuTableEndpoint::Geometry>);
static_assert(!std::same_as<RtuEndpoint::Format, RtuTableEndpoint::Format>);
static_assert(sizeof(RtuEndpoint::Message) == sizeof(RtuTableEndpoint::Message));
static_assert(sizeof(RtuEndpoint::Packet) == sizeof(RtuTableEndpoint::Packet));
static_assert(std::same_as<RtuEndpoint::Crc, ::crc::Crc16Bitwise>);
static_assert(std::same_as<RtuTableEndpoint::Crc, ::crc::Crc16Table>);
static_assert(std::same_as<RtuNoCrcEndpoint::Crc, ::crc::NoCrc>);
static_assert(RtuEndpoint::max_send_size == 252u);
static_assert(RtuNoCrcEndpoint::max_send_size == 254u);

static_assert(!HasRtuMetadata<typename CobsEndpoint::Packet>);
static_assert(HasRtuMetadata<typename RtuEndpoint::Packet>);
static_assert(HasStreamConsume<CobsEndpoint> && !HasAduReceive<CobsEndpoint>);
static_assert(!HasStreamConsume<RtuEndpoint> && HasAduReceive<RtuEndpoint>);

using U16Reader = bool (*)(
	std::span<const uint8_t>, std::size_t&, uint16_t&) noexcept;
constexpr U16Reader cobs_native = &cobs::read_native<uint16_t>;
constexpr U16Reader modbus_native = &modbus::read_native<uint16_t>;
constexpr U16Reader cobs_be = &cobs::read_be<uint16_t>;
constexpr U16Reader modbus_be = &modbus::read_be<uint16_t>;
constexpr U16Reader cobs_le = &cobs::read_le<uint16_t>;
constexpr U16Reader modbus_le = &modbus::read_le<uint16_t>;
using BytesReader = bool (*)(
	std::span<const uint8_t>, std::size_t&, std::size_t,
	std::span<const uint8_t>&) noexcept;
constexpr BytesReader cobs_bytes = &cobs::read_bytes;
constexpr BytesReader modbus_bytes = &modbus::read_bytes;

static_assert(cobs_native == modbus_native);
static_assert(cobs_be == modbus_be);
static_assert(cobs_le == modbus_le);
static_assert(cobs_bytes == modbus_bytes);

} // namespace

int main()
{
	std::puts("COBS/Modbus shared API contract passed");
	return 0;
}
