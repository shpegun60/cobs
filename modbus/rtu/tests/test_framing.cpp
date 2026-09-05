/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * framing::Layout and the standard function table, checked against the
 * worked examples of the Modbus Application Protocol Specification V1.1b3
 * §6 in BOTH directions, plus the exception form and the two functions that
 * carry no length indicator. Every example is the function DATA of a real
 * frame: `matches()` must accept it whole, `header_size()` must name exactly
 * the bytes needed before `data_size()` can answer, and that answer must be
 * the example's length.
 */
#include "modbus/rtu/Framing.h"
#include "Test.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

using namespace modbus_test;
namespace framing = modbus::rtu::framing;
using framing::Direction;
using framing::Layout;

namespace {

constexpr Direction kReq = Direction::Request;
constexpr Direction kResp = Direction::Response;

struct Example final {
	const char* name;
	uint8_t function;
	Direction direction;
	std::vector<uint8_t> data;
	std::size_t header;
};

void verify_example(const Example& example)
{
	const Layout layout = framing::standard_layout(example.direction, example.function);
	const std::span<const uint8_t> data{example.data};
	check(layout.supported(), example.name);
	check(layout.header_size() == example.header, example.name);
	check(data.size() >= layout.header_size() &&
	      layout.data_size(data.first(layout.header_size())) == data.size(),
	      example.name);
	check(layout.matches(data), example.name);
	// One byte more or less is not this frame.
	std::vector<uint8_t> longer = example.data;
	longer.push_back(0u);
	check(!layout.matches(longer), example.name);
	if (!example.data.empty()) {
		check(!layout.matches(data.first(data.size() - 1u)) ||
		      layout.header_size() > data.size() - 1u,
		      example.name);
	}
}

struct MyFramer : framing::Standard<Direction::Request> {
	using Base = framing::Standard<Direction::Request>;

	[[nodiscard]] static constexpr Layout layout(
			const Direction direction,
			const uint8_t function) noexcept
	{
		if (function == 0x41u) {
			return Layout::length_prefixed(2u);
		}
		if (function == 0x42u) {
			return direction == Direction::Request ? Layout::fixed(12u)
			                                       : Layout::fixed(0u);
		}
		return Base::layout(direction, function);
	}
};

struct NoLayout {
	static constexpr Direction rx = Direction::Request;
};

struct ThrowingLayout {
	static constexpr Direction rx = Direction::Request;
	static Layout layout(Direction, uint8_t) { return {}; }
};

static_assert(framing::Policy<MyFramer>);
static_assert(framing::Framer<MyFramer>);
static_assert(!framing::Policy<NoLayout>);
static_assert(!framing::Policy<ThrowingLayout>, "layout() must be noexcept");
static_assert(!framing::Framer<int>);

// The table is constexpr: a user can assert their own function set at
// compile time.
static_assert(framing::standard_layout(kReq, 0x03u) == Layout::fixed(4u));
static_assert(framing::standard_layout(kResp, 0x03u) == Layout::byte_count_at(0u));
static_assert(framing::standard_layout(kReq, 0x08u) == Layout::fixed(4u));
static_assert(!framing::standard_layout(kReq, 0x2Bu).supported());
static_assert(MyFramer::layout(kReq, 0x41u).reserved() == 2u);
static_assert(MyFramer::layout(kReq, 0x03u) == Layout::fixed(4u));

} // namespace

int main()
{
	group("LayoutConstructors");
	check(!Layout::unsupported().supported(), "unsupported() is not supported");
	check(Layout::fixed(0u).supported() && Layout::fixed(0u).size == 0u, "fixed(0)");
	check(Layout::fixed(65535u).size == 65535u, "fixed(65535) fits uint16");
	check(!Layout::fixed(65536u).supported(), "fixed(65536) is refused");
	check(Layout::byte_count_at(13u).supported() && Layout::byte_count_at(13u).header_size() == 14u,
	      "count byte at the last header position");
	check(!Layout::byte_count_at(13u, 2u).supported(), "a two-byte count must end within the header");
	check(!Layout::byte_count_at(0u, 3u).supported(), "count width is 1 or 2");
	check(!Layout::byte_count_at(0u, 0u).supported(), "count width is not 0");
	check(Layout::byte_count_at(4u).reserved() == 0u, "application-written counts are not reserved");
	check(Layout::length_prefixed(2u).owned && Layout::length_prefixed(2u).reserved() == 2u,
	      "length_prefixed(2) is library-owned");
	check(Layout::length_prefixed(1u).header_size() == 1u, "length_prefixed(1) header");
	check(!Layout::length_prefixed(3u).supported() && !Layout::length_prefixed(3u).owned,
	      "length_prefixed(3) is refused and not owned");
	check(Layout::fixed(4u).header_size() == 0u, "fixed needs no header bytes");
	check(!Layout::fixed(4u).matches(std::array<uint8_t, 3>{}), "fixed(4) rejects 3 bytes");
	check(!Layout::unsupported().matches(std::array<uint8_t, 0>{}), "unsupported matches nothing");

	group("CountFieldCodec");
	{
		std::array<uint8_t, 6> data{};
		const Layout be = Layout::length_prefixed(2u);
		be.store_count(data.data(), data.size());
		check(data[0] == 0u && data[1] == 4u, "BE16 count of the bytes after the field");
		check(be.data_size(data) == 6u && be.matches(data), "BE16 count reads back");
		const Layout le = Layout::length_prefixed(2u, std::endian::little);
		le.store_count(data.data(), data.size());
		check(data[0] == 4u && data[1] == 0u, "LE16 count");
		check(le.data_size(data) == 6u && le.matches(data), "LE16 count reads back");
		const Layout one = Layout::byte_count_at(1u);
		one.store_count(data.data(), data.size());
		check(data[1] == 4u && one.data_size(data) == 6u, "one-byte count at offset 1");
		std::array<uint8_t, 300> big{};
		Layout::length_prefixed(2u).store_count(big.data(), big.size());
		check(big[0] == 1u && big[1] == 42u && Layout::length_prefixed(2u).data_size(big) == 300u,
		      "counts above 255 need the second byte");
		std::array<uint8_t, 2> empty_body{};
		Layout::length_prefixed(2u).store_count(empty_body.data(), empty_body.size());
		check(empty_body[0] == 0u && empty_body[1] == 0u && Layout::length_prefixed(2u).matches(empty_body),
		      "an empty body has count 0");
	}

	group("StandardTableAgainstSpecificationExamples");
	const std::vector<Example> examples{
		{"01 Read Coils request", 0x01u, kReq, {0x00u, 0x13u, 0x00u, 0x13u}, 0u},
		{"01 Read Coils response", 0x01u, kResp, {0x03u, 0xCDu, 0x6Bu, 0x05u}, 1u},
		{"02 Read Discrete Inputs request", 0x02u, kReq, {0x00u, 0xC4u, 0x00u, 0x16u}, 0u},
		{"02 Read Discrete Inputs response", 0x02u, kResp, {0x03u, 0xACu, 0xDBu, 0x35u}, 1u},
		{"03 Read Holding Registers request", 0x03u, kReq, {0x00u, 0x6Bu, 0x00u, 0x03u}, 0u},
		{"03 Read Holding Registers response", 0x03u, kResp, {0x06u, 0x02u, 0x2Bu, 0x00u, 0x00u, 0x00u, 0x64u}, 1u},
		{"04 Read Input Registers request", 0x04u, kReq, {0x00u, 0x08u, 0x00u, 0x01u}, 0u},
		{"04 Read Input Registers response", 0x04u, kResp, {0x02u, 0x00u, 0x0Au}, 1u},
		{"05 Write Single Coil request", 0x05u, kReq, {0x00u, 0xACu, 0xFFu, 0x00u}, 0u},
		{"05 Write Single Coil response", 0x05u, kResp, {0x00u, 0xACu, 0xFFu, 0x00u}, 0u},
		{"06 Write Single Register request", 0x06u, kReq, {0x00u, 0x01u, 0x00u, 0x03u}, 0u},
		{"06 Write Single Register response", 0x06u, kResp, {0x00u, 0x01u, 0x00u, 0x03u}, 0u},
		{"07 Read Exception Status request", 0x07u, kReq, {}, 0u},
		{"07 Read Exception Status response", 0x07u, kResp, {0x6Du}, 0u},
		{"08 Diagnostics request (Return Query Data, two bytes)", 0x08u, kReq, {0x00u, 0x00u, 0xA5u, 0x37u}, 0u},
		{"08 Diagnostics response (Return Query Data echo)", 0x08u, kResp, {0x00u, 0x00u, 0xA5u, 0x37u}, 0u},
		{"08 Diagnostics request (Clear Counters)", 0x08u, kReq, {0x00u, 0x0Au, 0x00u, 0x00u}, 0u},
		{"08 Diagnostics response (Return Bus Message Count)", 0x08u, kResp, {0x00u, 0x0Bu, 0x01u, 0x2Cu}, 0u},
		{"0B Get Comm Event Counter request", 0x0Bu, kReq, {}, 0u},
		{"0B Get Comm Event Counter response", 0x0Bu, kResp, {0xFFu, 0xFFu, 0x01u, 0x08u}, 0u},
		{"0C Get Comm Event Log request", 0x0Cu, kReq, {}, 0u},
		{"0C Get Comm Event Log response", 0x0Cu, kResp, {0x06u, 0x00u, 0x00u, 0x01u, 0x08u, 0x20u, 0x00u}, 1u},
		{"0F Write Multiple Coils request", 0x0Fu, kReq, {0x00u, 0x13u, 0x00u, 0x0Au, 0x02u, 0xCDu, 0x01u}, 5u},
		{"0F Write Multiple Coils response", 0x0Fu, kResp, {0x00u, 0x13u, 0x00u, 0x0Au}, 0u},
		{"10 Write Multiple Registers request", 0x10u, kReq, {0x00u, 0x01u, 0x00u, 0x02u, 0x04u, 0x00u, 0x0Au, 0x01u, 0x02u}, 5u},
		{"10 Write Multiple Registers response", 0x10u, kResp, {0x00u, 0x01u, 0x00u, 0x02u}, 0u},
		{"11 Report Server ID request", 0x11u, kReq, {}, 0u},
		{"11 Report Server ID response", 0x11u, kResp, {0x02u, 0x2Au, 0xFFu}, 1u},
		{"14 Read File Record request", 0x14u, kReq,
			{0x0Eu, 0x06u, 0x00u, 0x04u, 0x00u, 0x01u, 0x00u, 0x02u, 0x06u, 0x00u, 0x03u, 0x00u, 0x09u, 0x00u, 0x02u}, 1u},
		{"14 Read File Record response", 0x14u, kResp,
			{0x0Cu, 0x05u, 0x06u, 0x0Du, 0xFEu, 0x00u, 0x20u, 0x05u, 0x06u, 0x33u, 0xCDu, 0x00u, 0x40u}, 1u},
		{"15 Write File Record request", 0x15u, kReq,
			{0x0Du, 0x06u, 0x00u, 0x04u, 0x00u, 0x07u, 0x00u, 0x03u, 0x06u, 0xAFu, 0x04u, 0xBEu, 0x10u, 0x0Du}, 1u},
		{"15 Write File Record response", 0x15u, kResp,
			{0x0Du, 0x06u, 0x00u, 0x04u, 0x00u, 0x07u, 0x00u, 0x03u, 0x06u, 0xAFu, 0x04u, 0xBEu, 0x10u, 0x0Du}, 1u},
		{"16 Mask Write Register request", 0x16u, kReq, {0x00u, 0x04u, 0x00u, 0xF2u, 0x00u, 0x25u}, 0u},
		{"16 Mask Write Register response", 0x16u, kResp, {0x00u, 0x04u, 0x00u, 0xF2u, 0x00u, 0x25u}, 0u},
		{"17 Read/Write Multiple Registers request", 0x17u, kReq,
			{0x00u, 0x03u, 0x00u, 0x06u, 0x00u, 0x0Eu, 0x00u, 0x03u, 0x06u, 0x00u, 0xFFu, 0x00u, 0xFFu, 0x00u, 0xFFu}, 9u},
		{"17 Read/Write Multiple Registers response", 0x17u, kResp,
			{0x0Cu, 0x00u, 0xFEu, 0x0Au, 0xCDu, 0x00u, 0x01u, 0x00u, 0x03u, 0x00u, 0x0Du, 0x00u, 0xFFu}, 1u},
		{"18 Read FIFO Queue request", 0x18u, kReq, {0x04u, 0xDEu}, 0u},
		{"18 Read FIFO Queue response (two-byte count)", 0x18u, kResp,
			{0x00u, 0x06u, 0x00u, 0x02u, 0x01u, 0xB8u, 0x12u, 0x84u}, 2u},
		{"exception response 0x83", 0x83u, kResp, {0x02u}, 0u},
		{"exception response 0xC1 (private base function)", 0xC1u, kResp, {0x04u}, 0u},
	};
	for (const Example& example : examples) {
		verify_example(example);
	}
	check(!framing::standard_layout(kReq, 0x83u).supported(), "there is no exception request");
	check(framing::standard_layout(kReq, 0x08u) == Layout::fixed(4u) &&
	      framing::standard_layout(kResp, 0x08u) == Layout::fixed(4u),
	      "08 Diagnostics is four bytes both ways, as in Qt Serial Bus");
	check(!framing::standard_layout(kReq, 0x08u).matches(std::array<uint8_t, 6>{0u, 0u, 1u, 2u, 3u, 4u}),
	      "a Return Query Data echo longer than two bytes does not frame (documented limitation)");
	check(!framing::standard_layout(kReq, 0x2Bu).supported() && !framing::standard_layout(kResp, 0x2Bu).supported(),
	      "2B Encapsulated Interface Transport carries no length indicator");
	check(!framing::standard_layout(kReq, 0x00u).supported() && !framing::standard_layout(kReq, 0x41u).supported() &&
	      !framing::standard_layout(kResp, 0x7Fu).supported(),
	      "unknown function codes are unsupported");
	{
		// Every supported request layout is a different frame from its response
		// wherever the specification says so.
		int asymmetric = 0;
		for (unsigned function = 1u; function < 0x80u; ++function) {
			const Layout request = framing::standard_layout(kReq, static_cast<uint8_t>(function));
			const Layout response = framing::standard_layout(kResp, static_cast<uint8_t>(function));
			check(request.supported() == response.supported(), "a function is known in both directions or neither");
			if (request.supported() && !(request == response)) {
				++asymmetric;
			}
		}
		check(asymmetric == 12, "12 standard functions have direction-dependent layouts (05/06/14/15/16 are symmetric)");
	}

	group("UserExtension");
	check(MyFramer::rx == Direction::Request, "inherits the direction");
	check(MyFramer::layout(kReq, 0x41u).owned && MyFramer::layout(kResp, 0x41u).owned,
	      "private function 0x41 is length-prefixed in both directions");
	check(MyFramer::layout(kReq, 0x42u) == Layout::fixed(12u) && MyFramer::layout(kResp, 0x42u) == Layout::fixed(0u),
	      "private function 0x42 has its own per-direction sizes");
	check(MyFramer::layout(kResp, 0x03u) == Layout::byte_count_at(0u), "standard functions fall through to the base");
	check(MyFramer::layout(kResp, 0xC1u) == Layout::fixed(1u), "the exception form of a private function is standard");
	check(!MyFramer::layout(kReq, 0x2Bu).supported(), "the base's unsupported functions stay unsupported");

	return finish();
}
