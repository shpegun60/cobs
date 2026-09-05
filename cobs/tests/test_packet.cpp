/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Public cobs::Packet lifetime contract.
 *
 * Only Cobs.h is included from production. A Packet can be created only by
 * the legitimate public path:
 *
 *     Endpoint::consume() -> ready queue -> Endpoint::pop_packet()
 *
 * Packet::adopt() stays private to detail::Receiver; this suite does not add a
 * friend or mint references by hand. Refcounts are checked behaviorally
 * through Pool occupancy: a block remains unavailable while any handle exists
 * and returns exactly once after the last one is released.
 */
#include "Cobs.h"
#include "reference_frame.h"

#include <cstdio>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void group(const char* name) { std::printf("\n[%s]\n", name); }

void check(const bool ok, const std::string& what)
{
	++g_checks;
	if (ok) {
		std::printf("  ok    %s\n", what.c_str());
	} else {
		++g_failures;
		std::printf("  FAIL  %s\n", what.c_str());
	}
}

constexpr std::size_t kMaxPayload = 64;
constexpr std::size_t kBlocks = 4;
using Engine = cobs::Endpoint<wire::Pool<kBlocks, 1>, cobs::Format<crc::NoCrc, kMaxPayload>>;
using Packet = Engine::Packet;

std::vector<uint8_t> payload(const uint8_t tag, const std::size_t n)
{
	std::vector<uint8_t> bytes(n);
	for (std::size_t i = 0; i < n; ++i) {
		bytes[i] = static_cast<uint8_t>(tag + i);
	}
	return bytes;
}

void feed(Engine& engine, const std::vector<uint8_t>& body)
{
	const auto wire = cobs_test::frame(body, Engine::length_size);
	engine.consume(std::span<const uint8_t>{wire});
}

bool matches(const Packet& packet, const std::vector<uint8_t>& expected)
{
	if (packet.size() != expected.size()) {
		return false;
	}
	const auto bytes = packet.data();
	for (std::size_t i = 0; i < expected.size(); ++i) {
		if (bytes[i] != expected[i]) {
			return false;
		}
	}
	return true;
}

void testPublicTypeContract()
{
	static_assert(std::is_copy_constructible_v<Packet>);
	static_assert(std::is_copy_assignable_v<Packet>);
	static_assert(std::is_nothrow_move_constructible_v<Packet>);
	static_assert(std::is_nothrow_move_assignable_v<Packet>);
	static_assert(std::is_same_v<
		decltype(std::declval<const Packet&>().data()),
		std::span<const uint8_t>>);

	const Packet empty;
	check(!empty && empty.size() == 0 && empty.data().empty(),
	      "a default packet is an empty nullable handle");
}

void testHandleSemantics()
{
	const auto body = payload(0xD0, 6);

	{
		Engine engine;
		feed(engine, body);
		check(engine.storage().rx_available() == kBlocks - 1,
		      "the ready queue holds one block");
		Packet a = engine.pop_packet();
		check(engine.storage().rx_available() == kBlocks - 1,
		      "pop transfers the queue reference without freeing or duplicating it");
		{
			Packet b = a;
			a.reset();
			check(engine.storage().rx_available() == kBlocks - 1,
			      "after a copy, releasing the original does not free the packet");
			check(matches(b, body), "and the copy still reads the payload");
		}
		check(engine.storage().rx_available() == kBlocks, "the last handle frees it");
		check(engine.storage().rx_stats().rejected == 0, "exactly once");
	}
	{
		Engine engine;
		feed(engine, body);
		Packet a = engine.pop_packet();
		Packet b = std::move(a);
		check(!a, "a moved-from handle is empty");
		check(matches(b, body), "the moved-to handle owns the packet");
		check(engine.storage().rx_available() == kBlocks - 1,
		      "and it is still held exactly once");
		b.reset();
		check(engine.storage().rx_available() == kBlocks, "releasing it frees the block");
	}
	{
		Engine engine;
		feed(engine, body);
		feed(engine, body);
		Packet a = engine.pop_packet();
		Packet b = engine.pop_packet();
		check(engine.storage().rx_available() == kBlocks - 2, "two packets held");
		b = a;
		check(engine.storage().rx_available() == kBlocks - 1,
		      "copy assignment released b's packet");
		a.reset();
		check(engine.storage().rx_available() == kBlocks - 1,
		      "b still holds the shared one");
		b.reset();
		check(engine.storage().rx_available() == kBlocks, "and releases it last");

		feed(engine, body);
		feed(engine, body);
		Packet c = engine.pop_packet();
		Packet d = engine.pop_packet();
		d = std::move(c);
		check(engine.storage().rx_available() == kBlocks - 1,
		      "move assignment released d's packet");
		check(matches(d, body) && !c, "and transferred c's");
	}
	{
		Engine engine;
		feed(engine, body);
		Packet a = engine.pop_packet();
		Packet& alias = a;

		a = alias;
		check(static_cast<bool>(a) && matches(a, body),
		      "self copy-assignment leaves the handle and its payload intact");
		check(engine.storage().rx_available() == kBlocks - 1,
		      "and the block stays held");

		a = std::move(alias);
		check(static_cast<bool>(a) && matches(a, body),
		      "self move-assignment leaves the handle and its payload intact");

		a.reset();
		check(engine.storage().rx_available() == kBlocks,
		      "and it still frees exactly once");
		check(engine.storage().rx_stats().rejected == 0, "with no double free");
	}
}

void testRetentionConsumesCapacity()
{
	Engine engine;
	const auto body = payload(0xF0, 2);

	std::vector<Packet> retained;
	for (std::size_t i = 0; i < kBlocks; ++i) {
		feed(engine, body);
		retained.push_back(engine.pop_packet());
	}
	check(engine.storage().rx_available() == 0, "retained packets consume the RX pool");

	feed(engine, body);
	check(engine.stats().rx.allocation_failure == 1,
	      "so a further frame cannot be received while they are held");

	retained.clear();
	check(engine.storage().rx_available() == kBlocks,
	      "releasing them restores capacity");
	feed(engine, body);
	check(static_cast<bool>(engine.pop_packet()), "and reception resumes");
}

void testPayloadReaders()
{
	Engine engine;
	const std::vector<uint8_t> body{
		0xA5u,
		0x12u, 0x34u,
		0x78u, 0x56u, 0x34u, 0x12u,
		0xCAu, 0xFEu,
	};
	feed(engine, body);
	const Packet packet = engine.pop_packet();

	std::size_t offset = 0u;
	uint8_t tag = 0u;
	uint16_t be16 = 0u;
	uint32_t le32 = 0u;
	std::span<const uint8_t> tail{};
	check(packet &&
	      cobs::read_native(packet.data(), offset, tag) && tag == 0xA5u &&
	      cobs::read_be(packet.data(), offset, be16) && be16 == 0x1234u &&
	      cobs::read_le(packet.data(), offset, le32) && le32 == 0x12345678u &&
	      cobs::read_bytes(packet.data(), offset, 2u, tail) &&
	      tail.size() == 2u && tail[0] == 0xCAu && tail[1] == 0xFEu &&
	      offset == packet.size(),
	      "COBS exposes the same stateless native/BE/LE/bytes reader vocabulary");

	const std::size_t old_offset = offset;
	uint16_t untouched_scalar = 0xBEEFu;
	const std::span<const uint8_t> old_tail = tail;
	check(!cobs::read_be(packet.data(), offset, untouched_scalar) &&
	      offset == old_offset && untouched_scalar == 0xBEEFu,
	      "a failed COBS scalar read changes neither cursor nor output");
	check(!cobs::read_bytes(packet.data(), offset, 1u, tail) &&
	      offset == old_offset && tail.data() == old_tail.data() &&
	      tail.size() == old_tail.size(),
	      "a failed COBS byte read changes neither cursor nor output view");
}

/*
 * The count is uint32_t. A former 16-bit count wrapped after 65536 copies and
 * freed a block while live handles still pointed at it. Staying beyond that
 * old boundary makes the public lifetime failure observable under ASan.
 */
void testRefcountDoesNotWrap()
{
	Engine engine;
	const auto body = payload(0x61, 4);
	feed(engine, body);
	Packet original = engine.pop_packet();
	check(static_cast<bool>(original), "one packet, one reference");

	constexpr std::size_t kCopies = 70000;
	{
		std::vector<Packet> copies;
		copies.reserve(kCopies);
		for (std::size_t i = 0; i < kCopies; ++i) {
			copies.emplace_back(original);
		}
		check(engine.storage().rx_available() == kBlocks - 1,
		      "70000 copies free nothing");

		copies.pop_back();
		check(engine.storage().rx_available() == kBlocks - 1,
		      "and dropping one of them frees nothing either");
		check(matches(original, body),
		      "the original still reads its own bytes, not freed memory");
	}
	check(engine.storage().rx_available() == kBlocks - 1,
	      "the packet outlives every copy but the original");
	original.reset();
	check(engine.storage().rx_available() == kBlocks,
	      "and is freed exactly once when the last one goes");
}

} // namespace

int main()
{
	group("PublicType");
	testPublicTypeContract();

	group("Ownership");
	testHandleSemantics();

	group("BackPressure");
	testRetentionConsumesCapacity();

	group("PayloadReaders");
	testPayloadReaders();

	group("RefcountWidth");
	testRefcountDoesNotWrap();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
