/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

// Independent framing oracle: no Layout helpers or library CRC are used to
// construct or validate the frames. Exercise every header depth, arbitrary
// stream segmentation, damaged declarations, gaps and retained pool owners.
#include "modbus/rtu/Rtu.h"
#include "Test.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <utility>
#include <vector>

using namespace modbus_test;
namespace framing = modbus::rtu::framing;

namespace {

struct Framer {
	static constexpr framing::Direction rx = framing::Direction::Request;
	static constexpr framing::Layout layout(framing::Direction, uint8_t function) noexcept
	{
		switch (function) {
		case 0x40u: return framing::Layout::fixed(0u);
		case 0x41u: return framing::Layout::length_prefixed(1u);
		case 0x42u: return framing::Layout::length_prefixed(2u);
		case 0x43u: return framing::Layout::length_prefixed(2u, std::endian::little);
		case 0x44u: return framing::Layout::byte_count_at(12u, 2u);
		default: return framing::Layout::unsupported();
		}
	}
};

uint16_t reference_crc(std::span<const uint8_t> bytes)
{
	uint16_t result = 0xFFFFu;
	for (const uint8_t byte : bytes) {
		result = static_cast<uint16_t>(result ^ byte);
		for (unsigned i = 0; i < 8u; ++i) {
			const bool low = (result & 1u) != 0u;
			result = static_cast<uint16_t>(result >> 1u);
			if (low) { result ^= 0xA001u; }
		}
	}
	return result;
}

std::vector<uint8_t> reference_frame(std::mt19937& random, std::size_t crc_size)
{
	const auto function = static_cast<uint8_t>(0x40u + random() % 5u);
	const std::size_t header = function == 0x40u ? 0u :
	                          function == 0x41u ? 1u : function == 0x44u ? 14u : 2u;
	const std::size_t body = function == 0x40u ? 0u :
	                        random() % (function == 0x41u ? 256u : 490u);
	std::vector<uint8_t> frame(2u + header + body);
	for (auto& byte : frame) { byte = static_cast<uint8_t>(random()); }
	frame[1] = function;
	if (header != 0u) {
		const std::size_t offset = function == 0x44u ? 14u : 2u;
		if (function == 0x41u) {
			frame[offset] = static_cast<uint8_t>(body);
		} else if (function == 0x43u) {
			frame[offset] = static_cast<uint8_t>(body);
			frame[offset + 1u] = static_cast<uint8_t>(body >> 8u);
		} else {
			frame[offset] = static_cast<uint8_t>(body >> 8u);
			frame[offset + 1u] = static_cast<uint8_t>(body);
		}
	}
	if (crc_size != 0u) {
		const uint16_t value = reference_crc(frame);
		frame.push_back(static_cast<uint8_t>(value));
		frame.push_back(static_cast<uint8_t>(value >> 8u));
	}
	return frame;
}

bool reference_valid(std::span<const uint8_t> frame, std::size_t crc_size)
{
	if (frame.size() < 2u + crc_size || frame.size() > 512u + 2u + crc_size) { return false; }
	if (crc_size != 0u) {
		const std::size_t body = frame.size() - 2u;
		const auto value = static_cast<uint16_t>(frame[body] |
			(static_cast<uint16_t>(frame[body + 1u]) << 8u));
		if (reference_crc(frame.first(body)) != value) { return false; }
	}
	const auto data = frame.subspan(2u, frame.size() - 2u - crc_size);
	switch (frame[1]) {
	case 0x40u: return data.empty();
	case 0x41u: return !data.empty() && data.size() == 1u + data[0];
	case 0x42u: return data.size() >= 2u && data.size() == 2u + 256u * data[0] + data[1];
	case 0x43u: return data.size() >= 2u && data.size() == 2u + data[0] + 256u * data[1];
	case 0x44u: return data.size() >= 14u && data.size() == 14u + 256u * data[12] + data[13];
	default: return false;
	}
}

template<class Crc, class Memory>
bool valid_streams()
{
	using Link = modbus::rtu::Endpoint<Memory, modbus::rtu::Format<Crc, 512>, Framer>;
	std::mt19937 random{0x52545811u};
	Link link;
	for (unsigned run = 0u; run < 2000u; ++run) {
		std::vector<std::vector<uint8_t>> expected;
		std::vector<uint8_t> stream;
		const unsigned count = 1u + static_cast<unsigned>(random() % 8u);
		for (unsigned n = 0u; n < count; ++n) {
			expected.push_back(reference_frame(random, Crc::wire_size));
			if (!reference_valid(expected.back(), Crc::wire_size)) { return false; }
			stream.insert(stream.end(), expected.back().begin(), expected.back().end());
		}
		std::span<const uint8_t> remaining{stream};
		while (!remaining.empty()) {
			const auto cut = std::min(remaining.size(), std::size_t{1u + random() % 600u});
			link.consume({}); // empty input never changes assembly
			link.consume(remaining.first(cut));
			remaining = remaining.subspan(cut);
		}
		for (const auto& frame : expected) {
			const auto packet = link.pop_packet();
			if (!packet || !equal(packet.adu(), frame)) { return false; }
		}
		if (link.assembling() || link.has_packet() || link.stats().rx.crc_errors != 0u ||
		    link.framing_stats().resyncs != 0u) { return false; }
	}
	return true;
}

template<class Crc>
bool damaged_streams()
{
	using Link = modbus::rtu::Endpoint<wire::Pool<3, 2>, modbus::rtu::Format<Crc, 512>, Framer>;
	std::mt19937 random{0x47415011u};
	Link link;
	std::vector<typename Link::Packet> held;
	for (unsigned run = 0u; run < 50000u; ++run) {
		switch (random() % 9u) {
		case 0u: link.notify_gap(); break;
		case 1u: link.expire_incomplete(); break;
		case 2u: if (!held.empty()) { held.pop_back(); } break;
		default: {
			auto frame = reference_frame(random, Crc::wire_size);
			if (random() % 2u != 0u) {
				frame[random() % frame.size()] ^= static_cast<uint8_t>(1u << (random() % 8u));
			}
			if (random() % 2u != 0u) { frame.resize(random() % (frame.size() + 1u)); }
			const auto cut = random() % (frame.size() + 1u);
			link.consume(std::span<const uint8_t>{frame}.first(cut));
			link.consume(std::span<const uint8_t>{frame}.subspan(cut));
			break;
		}
		}
		while (auto packet = link.pop_packet()) {
			if (!reference_valid(packet.adu(), Crc::wire_size)) { return false; }
			if (held.size() < 3u && random() % 2u != 0u) { held.push_back(std::move(packet)); }
		}
		if (link.storage().rx_available() + held.size() > 3u ||
		    link.storage().rx_stats().rejected != 0u) { return false; }
	}
	held.clear();
	link.discard_incomplete();
	return link.storage().rx_available() == 3u && link.storage().rx_stats().rejected == 0u &&
	       link.stats().rx.allocation_failure != 0u && link.framing_stats().skipped_frames != 0u;
}

template<class Crc>
void run_policy()
{
	check(valid_streams<Crc, wire::Heap>(), "2,000 independent frame trains: Heap, arbitrary cuts and glued frames");
	check(valid_streams<Crc, wire::Pool<8, 2>>(), "same 2,000 frame trains: Pool, exact packet bytes and no loss");
	check(damaged_streams<Crc>(), "50,000 damaged-stream operations: oracle, gaps, expiry, exhaustion, retained owners");
}

} // namespace

int main()
{
	group("RandomFramingNoCrc");
	run_policy<::crc::NoCrc>();
	group("RandomFramingBitwise");
	run_policy<::crc::Crc16Bitwise>();
	group("RandomFramingTable");
	run_policy<::crc::Crc16Table>();
	return finish();
}
