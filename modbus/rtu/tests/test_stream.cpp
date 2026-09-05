/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Endpoint<Memory, Format, Framer>: the stream receiver assembled from a
 * framing policy, and the builder side of the same layout table.
 *
 * Every frame here is cut at every position, glued to its neighbours, or
 * damaged in a way the length rule must classify; the recovery rule
 * (Framing.h) is exercised on every error class; storage is checked for
 * leaks after each scenario. The default endpoint is proven untouched by
 * type identity and size.
 */
#include "modbus/rtu/Rtu.h"
#include "Test.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

using namespace modbus_test;
namespace framing = modbus::rtu::framing;
using framing::Direction;
using framing::Layout;

namespace {

struct Transport final {
	bool busy_state = false;
	std::vector<uint8_t> frame{};

	bool send(const std::span<const uint8_t> bytes) noexcept
	{
		frame.assign(bytes.begin(), bytes.end());
		busy_state = true;
		return true;
	}

	bool busy() const noexcept { return busy_state; }
};

template<class Endpoint>
void bind(Endpoint& endpoint, Transport& transport)
{
	check(endpoint.bind(
		typename Endpoint::Sender{tiny::bind<&Transport::send>(transport)},
		typename Endpoint::BusyQuery{tiny::bind<&Transport::busy>(transport)}), "bind");
}

template<class Endpoint>
concept HasConsume = requires(Endpoint& endpoint, std::span<const uint8_t> bytes) {
	endpoint.consume(bytes);
};

template<class Endpoint>
concept HasFramingStats = requires(const Endpoint& endpoint) { endpoint.framing_stats(); };

std::vector<uint8_t> concat(std::initializer_list<std::span<const uint8_t>> parts)
{
	std::vector<uint8_t> result;
	for (const std::span<const uint8_t> part : parts) {
		result.insert(result.end(), part.begin(), part.end());
	}
	return result;
}

template<class Endpoint>
std::vector<std::vector<uint8_t>> drain(Endpoint& endpoint)
{
	std::vector<std::vector<uint8_t>> frames;
	while (endpoint.has_packet()) {
		const typename Endpoint::Packet packet = endpoint.pop_packet();
		frames.emplace_back(packet.adu().begin(), packet.adu().end());
	}
	return frames;
}

// A private function with a library-owned length prefix, in both directions.
struct PrivateFramer : framing::Standard<Direction::Request> {
	using Base = framing::Standard<Direction::Request>;

	[[nodiscard]] static constexpr Layout layout(
			const Direction direction,
			const uint8_t function) noexcept
	{
		return function == 0x41u ? Layout::length_prefixed(2u)
		                         : Base::layout(direction, function);
	}
};

struct PrivatePeer : framing::Standard<Direction::Response> {
	using Base = framing::Standard<Direction::Response>;

	[[nodiscard]] static constexpr Layout layout(
			const Direction direction,
			const uint8_t function) noexcept
	{
		return function == 0x41u ? Layout::length_prefixed(2u)
		                         : Base::layout(direction, function);
	}
};

using Memory = wire::Pool<4, 1>;
using Plain = modbus::rtu::Endpoint<Memory>;
using Server = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>, framing::Standard<Direction::Request>>;
using Client = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>, framing::Standard<Direction::Response>>;
using TableServer = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<::crc::Crc16Table>, framing::Standard<Direction::Request>>;

// The framer changes neither the ownership types nor the default endpoint.
static_assert(std::is_same_v<Plain, modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>, framing::None>>);
static_assert(std::is_same_v<Plain::Packet, Server::Packet>);
static_assert(std::is_same_v<Plain::Message, Server::Message>);
static_assert(std::is_same_v<Plain::Storage, Server::Storage>);
static_assert(std::is_same_v<Server::Packet, TableServer::Packet>);
static_assert(!Plain::framed && Server::framed);
static_assert(sizeof(Server) > sizeof(Plain), "the stream state exists only when a framer is selected");
static_assert(sizeof(Server) == sizeof(TableServer));
static_assert(!HasConsume<Plain>, "consume() does not exist without a framing policy");
static_assert(!HasFramingStats<Plain>);
static_assert(HasConsume<Server> && HasFramingStats<Server>);

const std::vector<uint8_t> kReadReq = make_adu(0x11u, 0x03u, std::array<uint8_t, 4>{0x00u, 0x6Bu, 0x00u, 0x03u});
const std::vector<uint8_t> kWriteReq = make_adu(0x11u, 0x10u,
	std::array<uint8_t, 9>{0x00u, 0x01u, 0x00u, 0x02u, 0x04u, 0x00u, 0x0Au, 0x01u, 0x02u});
const std::vector<uint8_t> kCoilReq = make_adu(0x11u, 0x05u, std::array<uint8_t, 4>{0x00u, 0xACu, 0xFFu, 0x00u});
const std::vector<uint8_t> kStatusReq = make_adu(0x11u, 0x07u);
const std::vector<uint8_t> kReadResp = make_adu(0x11u, 0x03u,
	std::array<uint8_t, 7>{0x06u, 0x02u, 0x2Bu, 0x00u, 0x00u, 0x00u, 0x64u});
const std::vector<uint8_t> kExceptionResp = make_adu(0x11u, 0x83u, std::array<uint8_t, 1>{0x02u});
// 0x2B Encapsulated Interface Transport has no layout in the standard table.
const std::vector<uint8_t> kUnsupportedReq = make_adu(0x11u, 0x2Bu, std::array<uint8_t, 4>{0x0Eu, 0x01u, 0x00u, 0x00u});

} // namespace

int main()
{
	group("EveryCutOfOneFrame");
	{
		Server server;
		const std::span<const uint8_t> frame{kWriteReq};
		for (std::size_t cut = 0u; cut < frame.size(); ++cut) {
			server.consume(frame.first(cut));
			check(!server.has_packet(), "no packet before the last byte");
			server.consume(frame.subspan(cut));
			const auto frames = drain(server);
			check(frames.size() == 1u && equal(frames[0], frame), "one frame from two chunks");
		}
		for (const uint8_t byte : frame) {
			server.consume(std::span<const uint8_t>{&byte, 1u});
		}
		const auto frames = drain(server);
		check(frames.size() == 1u && equal(frames[0], frame), "one frame byte by byte");
		const modbus::rtu::Stats stats = server.stats();
		check(stats.rx.frames_received == frame.size() + 1u && stats.rx.candidates == frame.size() + 1u,
		      "every delivery counted, nothing else");
		check(server.framing_stats().resyncs == 0u && server.framing_stats().unsupported_function == 0u,
		      "no error on a clean stream");
		check(server.storage().rx_available() == 4u, "all RX blocks returned");
	}

	group("SeveralFramesPerChunk");
	{
		Server server;
		const std::vector<uint8_t> chunk = concat({kReadReq, kWriteReq, kCoilReq, kStatusReq});
		server.consume(chunk);
		const auto frames = drain(server);
		check(frames.size() == 4u, "four frames from one chunk");
		check(frames.size() == 4u && equal(frames[0], kReadReq) && equal(frames[1], kWriteReq) &&
		      equal(frames[2], kCoilReq) && equal(frames[3], kStatusReq),
		      "in order and byte-exact");
		// A frame straddling two chunks between two whole ones.
		const std::span<const uint8_t> write{kWriteReq};
		server.consume(concat({kReadReq, write.first(6u)}));
		check(drain(server).size() == 1u, "the whole frame is delivered while the straddling one waits");
		server.consume(concat({write.subspan(6u), kCoilReq}));
		const auto rest = drain(server);
		check(rest.size() == 2u && equal(rest[0], kWriteReq) && equal(rest[1], kCoilReq),
		      "the straddling frame completes and the next follows");
		check(server.storage().rx_available() == 4u, "all RX blocks returned");
	}

	group("UnsupportedFunctionDropsTheRestOfTheChunk");
	{
		Server server;
		server.consume(concat({kUnsupportedReq, kReadReq}));
		check(!server.has_packet(), "the frame after an unknown function is lost with the chunk");
		check(server.framing_stats().unsupported_function == 1u && server.framing_stats().resyncs == 1u,
		      "counted as unsupported plus one resync");
		server.consume(kReadReq);
		check(drain(server).size() == 1u, "the next chunk starts a frame");
		server.consume(kUnsupportedReq);
		check(server.framing_stats().unsupported_function == 2u && server.framing_stats().resyncs == 2u,
		      "the offending frame's own remainder is dropped and counted");
		server.consume(std::span<const uint8_t>{kUnsupportedReq}.first(2u));
		check(server.framing_stats().unsupported_function == 3u && server.framing_stats().resyncs == 2u,
		      "an error on the last byte of a chunk drops nothing else");
		// Exception frames are responses: a request-side receiver has no layout.
		server.consume(kExceptionResp);
		check(server.framing_stats().unsupported_function == 4u, "no exception requests");
		check(server.stats().rx.frames_received == 1u, "only the clean frame was delivered");
	}

	group("OversizeDeclaration");
	{
		Server server;
		// 0x10 declaring 250 data bytes: 2 + 5 + 250 + 2 = 259 > 256.
		std::vector<uint8_t> header{0x11u, 0x10u, 0x00u, 0x01u, 0x00u, 0x7Du, 0xFAu};
		server.consume(concat({header, kReadReq}));
		check(server.stats().rx.oversize == 1u && server.framing_stats().resyncs == 1u && !server.has_packet(),
		      "oversize is decided from the header; the chunk remainder is dropped");
		server.consume(kReadReq);
		check(drain(server).size() == 1u, "recovered on the next chunk");
		// The largest legal declaration, exactly 256 bytes, is accepted:
		// 2 + (5 + 247) + 2.
		std::vector<uint8_t> data(5u + 247u, 0u);
		data[0] = 0x00u; data[1] = 0x01u; data[2] = 0x00u; data[3] = 0x7Bu; data[4] = 0xF7u;
		const std::vector<uint8_t> maximum = make_adu(0x11u, 0x10u, data);
		check(maximum.size() == 256u, "maximum ADU built");
		server.consume(std::span<const uint8_t>{maximum}.first(100u));
		server.consume(std::span<const uint8_t>{maximum}.subspan(100u));
		const auto frames = drain(server);
		check(frames.size() == 1u && equal(frames[0], maximum), "256-byte frame assembled");
	}

	group("CrcFailureDropsTheRestOfTheChunk");
	{
		Server server;
		std::vector<uint8_t> corrupted = kWriteReq;
		corrupted[7] ^= 0x01u;
		server.consume(concat({corrupted, kReadReq}));
		check(server.stats().rx.crc_errors == 1u && server.framing_stats().resyncs == 1u && !server.has_packet(),
		      "a CRC failure cannot vouch for the frame boundary");
		server.consume(concat({kReadReq, corrupted}));
		check(drain(server).size() == 1u && server.stats().rx.crc_errors == 2u && server.framing_stats().resyncs == 1u,
		      "a leading good frame is delivered; a trailing bad one drops nothing else");
		check(server.storage().rx_available() == 4u, "rejected frames return their block");
	}

	group("AllocationFailureSkipsExactlyOneFrame");
	{
		using Tiny = modbus::rtu::Endpoint<wire::Pool<1, 1>, modbus::rtu::Format<>, framing::Standard<Direction::Request>>;
		Tiny server;
		const std::span<const uint8_t> write{kWriteReq};
		server.consume(concat({kReadReq, write.first(8u)}));
		check(server.has_packet() && server.stats().rx.allocation_failure == 1u &&
		      server.framing_stats().skipped_frames == 1u,
		      "the second frame is skipped once its length is known");
		const Tiny::Packet held = server.pop_packet();
		server.consume(concat({write.subspan(8u), kCoilReq}));
		check(server.framing_stats().resyncs == 0u, "skipping is not a resync");
		check(!server.has_packet(), "the skipped frame's tail is swallowed and the pool is still held");
		check(server.framing_stats().skipped_frames == 2u && server.stats().rx.allocation_failure == 2u,
		      "the third frame is skipped too while the packet is held");
		held.~Packet();
		new (const_cast<Tiny::Packet*>(&held)) Tiny::Packet{};
		server.consume(kCoilReq);
		const auto frames = drain(server);
		check(frames.size() == 1u && equal(frames[0], kCoilReq), "in step again after the block is back");
		check(server.stats().rx.frames_received == 2u, "two frames delivered in total");
	}

	group("GapDuringAFrame");
	{
		Server server;
		const std::span<const uint8_t> write{kWriteReq};
		server.consume(write.first(9u));
		check(server.storage().rx_available() == 3u, "a block is held while the frame is in flight");
		server.notify_gap();
		check(server.storage().rx_available() == 4u && server.stats().rx.stream_gaps == 1u,
		      "the in-flight block is released on a gap");
		server.consume(kReadReq);
		const auto frames = drain(server);
		check(frames.size() == 1u && equal(frames[0], kReadReq), "the next chunk is a fresh frame");
		server.consume(write.first(3u));
		server.notify_gap();
		server.consume(kReadReq);
		check(drain(server).size() == 1u, "a gap during the header is absorbed too");
	}

	group("CompleteCandidatesUnderAFramer");
	{
		Server server;
		server.receive_adu(kReadReq);
		check(drain(server).size() == 1u, "a matching candidate is accepted");
		server.receive_adu(make_adu(0x11u, 0x03u, std::array<uint8_t, 5>{0u, 1u, 2u, 3u, 4u}));
		check(!server.has_packet() && server.framing_stats().length_mismatch == 1u,
		      "a valid-CRC candidate of the wrong length is refused");
		server.receive_adu(kUnsupportedReq);
		check(server.framing_stats().unsupported_function == 1u, "an unsupported function is refused");
		server.receive_adu(make_adu(0x11u, 0x08u, std::array<uint8_t, 4>{0x00u, 0x0Au, 0x00u, 0x00u}));
		check(drain(server).size() == 1u, "a four-byte Diagnostics request is accepted");
		const std::array<uint8_t, 3> short_frame{0x11u, 0x03u, 0x00u};
		server.receive_adu(short_frame);
		check(server.stats().rx.too_short == 1u, "too-short candidates are still classified by the base");
		check(server.stats().rx.candidates == 5u, "every candidate counted once");
	}

	group("ResponseDirection");
	{
		Client client;
		client.consume(concat({kReadResp, kExceptionResp}));
		const auto frames = drain(client);
		check(frames.size() == 2u && equal(frames[0], kReadResp) && equal(frames[1], kExceptionResp),
		      "responses and exceptions are framed on the client side");
		// A request arriving at a response-side receiver: the byte-count rule
		// reads 0x00 and ends the frame after five bytes, where the CRC fails.
		client.consume(kReadReq);
		check(!client.has_packet() && client.stats().rx.crc_errors == 1u,
		      "a wrong-direction frame is refused by CRC, not delivered");
		client.consume(kWriteReq);
		check(!client.has_packet(), "a wrong-direction write request is refused too");
	}

	group("ZeroDataWithoutCrc");
	{
		using Bare = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<::crc::NoCrc>, framing::Standard<Direction::Request>>;
		Bare server;
		const std::array<uint8_t, 2> status{0x11u, 0x07u};
		server.consume(std::span<const uint8_t>{status}.first(1u));
		check(!server.has_packet(), "one byte is not a frame");
		server.consume(std::span<const uint8_t>{status}.subspan(1u));
		const auto frames = drain(server);
		check(frames.size() == 1u && frames[0].size() == 2u, "a two-byte ADU completes on its second byte");
		server.consume(concat({status, status, status}));
		check(drain(server).size() == 3u, "three two-byte ADUs in one chunk");
	}

	group("BuilderSideOfTheTable");
	{
		Server server;
		Transport transport;
		bind(server, transport);
		// A server sends responses: 0x03 is a byte count plus data.
		Server::Message response = server.make_message(0x11u, 0x03u);
		check(response.size() == 0u, "standard byte counts are written by the application");
		check(response.append_be<uint8_t>(6u) && response.append_be<uint16_t>(0x022Bu) &&
		      response.append_be<uint16_t>(0u) && response.append_be<uint16_t>(0x64u), "append");
		check(server.send(response) == modbus::SendResult::Sent, "a consistent response is sent");
		check(equal(transport.frame, kReadResp), "byte-identical to the reference frame");
		transport.busy_state = false;
		server.poll();

		Server::Message wrong = server.make_message(0x11u, 0x03u);
		check(wrong.append_be<uint8_t>(6u) && wrong.append_be<uint16_t>(0x022Bu), "count 6, two bytes of data");
		check(server.send(wrong) == modbus::SendResult::Invalid && server.framing_stats().tx_layout_rejected == 1u,
		      "a byte count that disagrees with the data is refused before the wire");
		check(!server.tx_active() && wrong, "nothing was surrendered; the caller keeps the message");
		wrong = Server::Message{}; // the single TX block goes back

		Server::Message coil = server.make_message(0x11u, 0x05u, 0u);
		check(coil && coil.capacity() >= 4u, "a fixed layout pre-allocates its size");
		check(coil.append_be<uint16_t>(0x00ACu), "two of four bytes");
		check(server.send(coil) == modbus::SendResult::Invalid && server.framing_stats().tx_layout_rejected == 2u,
		      "a short fixed response is refused");
		check(coil.append_be<uint16_t>(0xFF00u) && server.send(coil) == modbus::SendResult::Sent,
		      "the refused message is still usable and is sent once complete");
		check(equal(transport.frame, kCoilReq), "write single coil echo");
		transport.busy_state = false;
		server.poll();

		Server::Message exception = server.make_message(0x11u, 0x83u);
		check(exception.append_be<uint8_t>(0x02u) && server.send(exception) == modbus::SendResult::Sent &&
		      equal(transport.frame, kExceptionResp), "exception responses are fixed(1)");
		transport.busy_state = false;
		server.poll();

		Server::Message encapsulated = server.make_message(0x11u, 0x2Bu);
		check(encapsulated.append_be<uint32_t>(0x0E010000u) && server.send(encapsulated) == modbus::SendResult::Sent,
		      "a function the policy has no layout for is sent as the application built it");
		transport.busy_state = false;
		server.poll();
		check(server.framing_stats().tx_layout_rejected == 2u, "no further rejections");
	}

	group("LibraryOwnedLengthPrefix");
	{
		using Device = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>, PrivateFramer>;
		using Host = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<>, PrivatePeer>;
		Device device;
		Host host;
		Transport wire;
		bind(device, wire);

		Device::Message reply = device.make_message(0x11u, 0x41u);
		check(reply.size() == 2u && reply.capacity() >= 2u, "the prefix is reserved before the first append");
		const std::array<uint8_t, 5> body{0x41u, 0x42u, 0x43u, 0x44u, 0x45u};
		check(reply.append_bytes(body), "the application appends only the body");
		check(device.send(reply) == modbus::SendResult::Sent, "sent");
		check(wire.frame.size() == 2u + 2u + 5u + 2u && wire.frame[2] == 0u && wire.frame[3] == 5u,
		      "the prefix holds the body length at send time");
		host.consume(wire.frame);
		{
			const Host::Packet packet = host.pop_packet();
			check(static_cast<bool>(packet) && packet.function() == 0x41u && packet.size() == 7u &&
			      packet.data()[0] == 0u && packet.data()[1] == 5u && equal(packet.data().subspan(2u), body),
			      "the peer frames it from the prefix and data() shows [N][body]");
		}
		wire.busy_state = false;
		device.poll();

		// The hazard the prefix removes: nobody can forget the length word,
		// but a habitual extra one is just body and is counted as such.
		Device::Message doubled = device.make_message(0x11u, 0x41u);
		check(doubled.append_be<uint16_t>(5u) && doubled.append_bytes(body) &&
		      device.send(doubled) == modbus::SendResult::Sent &&
		      wire.frame[2] == 0u && wire.frame[3] == 7u,
		      "an application-written length becomes two body bytes; the prefix says 7");
		wire.busy_state = false;
		device.poll();

		Device::Message empty = device.make_message(0x11u, 0x41u);
		check(device.send(empty) == modbus::SendResult::Sent && wire.frame.size() == 6u &&
		      wire.frame[2] == 0u && wire.frame[3] == 0u,
		      "an empty body is a zero prefix");
		host.consume(wire.frame);
		check(host.pop_packet().size() == 2u, "the peer sees exactly the prefix");
		wire.busy_state = false;
		device.poll();

		// Growth keeps the prefix.
		Device::Message grown = device.make_message(0x11u, 0x41u, 2u);
		std::vector<uint8_t> big(200u, 0x5Au);
		check(grown.append_bytes(big) && grown.size() == 202u, "grown past the hint");
		check(device.send(grown) == modbus::SendResult::Sent && wire.frame[2] == 0u && wire.frame[3] == 200u,
		      "the prefix survives reallocation and is filled last");
		host.consume(std::span<const uint8_t>{wire.frame}.first(3u));
		host.consume(std::span<const uint8_t>{wire.frame}.subspan(3u));
		check(host.pop_packet().size() == 202u, "a split prefix frames correctly");
		wire.busy_state = false;
		device.poll();

		// A retry after a transport failure neither rewrites the prefix nor
		// re-checks: the finalized bytes are resent as they were.
		struct Refusing final {
			bool refuse = true;
			std::vector<uint8_t> frame{};
			bool send(std::span<const uint8_t> b) noexcept
			{
				if (refuse) { return false; }
				frame.assign(b.begin(), b.end());
				return true;
			}
			bool busy() const noexcept { return false; }
		} refusing;
		Device retry_device;
		check(retry_device.bind(
			Device::Sender{tiny::bind<&Refusing::send>(refusing)},
			Device::BusyQuery{tiny::bind<&Refusing::busy>(refusing)}), "bind refusing transport");
		Device::Message retried = retry_device.make_message(0x11u, 0x41u);
		check(retried.append_bytes(body) && retry_device.send(retried) == modbus::SendResult::Failed, "first attempt fails");
		refusing.refuse = false;
		check(retry_device.send(retried) == modbus::SendResult::Sent && refusing.frame[3] == 5u, "retry resends the same frame");

		check(device.storage().tx_available() == 1u && host.storage().rx_available() == 4u, "no leaks");
	}

	group("HeaderSplitAtEveryPosition");
	{
		// 0x17 has the deepest standard header (count at byte 8): every split of
		// the 11 prefix bytes must resume correctly.
		Server server;
		const std::vector<uint8_t> frame = make_adu(0x11u, 0x17u,
			std::array<uint8_t, 15>{0x00u, 0x03u, 0x00u, 0x06u, 0x00u, 0x0Eu, 0x00u, 0x03u, 0x06u,
			                        0x00u, 0xFFu, 0x00u, 0xFFu, 0x00u, 0xFFu});
		const std::span<const uint8_t> bytes{frame};
		for (std::size_t first = 1u; first < 11u; ++first) {
			for (std::size_t second = first + 1u; second <= 11u; ++second) {
				server.consume(bytes.first(first));
				server.consume(bytes.subspan(first, second - first));
				server.consume(bytes.subspan(second));
				const auto frames = drain(server);
				check(frames.size() == 1u && equal(frames[0], frame), "three-way header split");
			}
		}
		check(server.framing_stats().resyncs == 0u && server.stats().rx.crc_errors == 0u, "no errors");
	}

	return finish();
}
