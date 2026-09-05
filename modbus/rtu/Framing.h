/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Optional RTU framing policies: how the endpoint finds the END of an ADU in a
 * byte stream, given only its beginning.
 *
 *     modbus::rtu::Endpoint<Memory, Format>                      // framing::None
 *     modbus::rtu::Endpoint<Memory, Format, framing::Standard<Direction>>
 *
 * The default, framing::None, is the unchanged RTU contract: receive_adu()
 * takes one complete burst candidate and no function code is interpreted. A
 * framing policy adds consume(): arbitrary stream chunks, possibly holding a
 * fragment of an ADU or several ADUs, are assembled from a per-function
 * declaration of where the data length is encoded.
 *
 * That declaration is a framing::Layout and it is the single source of truth
 * for both directions. The receiver reads it to know how many bytes to expect;
 * the message builder reads it to reserve and fill a library-owned length
 * prefix, and to refuse a message whose data disagrees with its layout. The
 * two cannot drift apart, and a length field can be neither forgotten nor
 * miscounted by the application.
 *
 * Standard<Direction> is the table of the Modbus application-protocol
 * functions whose length follows from their own header. Direction is which
 * side of the exchange this endpoint RECEIVES: function 0x03 is 4 fixed bytes
 * as a request and a byte count plus data as a response, so the code alone
 * does not select a rule. Functions whose data carries no length indicator
 * (0x08 Diagnostics, 0x2B Encapsulated Interface Transport) are Unsupported:
 * they need context this layer does not have, exactly as Qt Serial Bus
 * documents for its own calculators.
 *
 * Private functions extend the table by inheritance:
 *
 *     struct MyFramer : modbus::rtu::framing::Standard<Direction::Request> {
 *         using Base = Standard<Direction::Request>;
 *         static constexpr framing::Layout layout(
 *                 const framing::Direction direction,
 *                 const uint8_t function) noexcept
 *         {
 *             if (function == 0x41u) {
 *                 // function-data = [length: BE16][body: length bytes]
 *                 return framing::Layout::length_prefixed(2u);
 *             }
 *             return Base::layout(direction, function);
 *         }
 *     };
 *
 * Recovery rule. Length-based framing can find the end of a frame only when
 * it knows the frame's start. After an error the receiver therefore drops the
 * remainder of the current chunk and treats the first byte of the NEXT chunk
 * as a frame start. This relies on the transport delivering chunks that are
 * eventually aligned to an inter-frame pause (the UART adapter's IDLE
 * boundary), which is the same assumption receive_adu() has always made; it
 * is a precondition, not a timing implementation of t1.5/t3.5. The one error
 * that keeps synchronization is an RX allocation failure: the length is known
 * by then, so exactly that frame is skipped.
 */

#ifndef MODBUS_RTU_FRAMING_H_
#define MODBUS_RTU_FRAMING_H_

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace modbus::rtu::framing {

// Which side of the exchange an endpoint RECEIVES. It transmits the opposite.
enum class Direction : uint8_t { Request, Response };

[[nodiscard]] constexpr Direction opposite(const Direction direction) noexcept
{
	return direction == Direction::Request ? Direction::Response
	                                       : Direction::Request;
}

/*
 * Where one function's data length is encoded, in function-data coordinates
 * (byte 0 is the first byte after the function code).
 *
 *     fixed(n)                      exactly n data bytes
 *     byte_count_at(offset, width)  a count of the bytes FOLLOWING the field,
 *                                   written by the application (standard Modbus
 *                                   byte counts)
 *     length_prefixed(width)        the same count at offset 0, but reserved
 *                                   and filled by the message builder: the
 *                                   application never touches it
 *     unsupported()                 no rule; such a frame is refused
 */
struct Layout final {
	enum class Kind : uint8_t { Unsupported, Fixed, Counted };

	// The count field must end within this many data bytes: the receiver
	// collects the header into a small fixed buffer before it knows how much
	// to allocate. The largest standard header (0x17 request) ends at byte 9.
	static constexpr std::size_t max_header_size = 14u;
	static constexpr std::size_t max_fixed_size = UINT16_MAX;

	Kind kind = Kind::Unsupported;
	bool owned = false;                     // Counted at offset 0 the builder fills
	uint8_t offset = 0u;                    // Counted: first byte of the count field
	uint8_t width = 0u;                     // Counted: 1 or 2
	std::endian order = std::endian::big;   // Counted, width 2
	uint16_t size = 0u;                     // Fixed

	[[nodiscard]] static constexpr Layout unsupported() noexcept { return {}; }

	[[nodiscard]] static constexpr Layout fixed(const std::size_t data_size) noexcept
	{
		if (data_size > max_fixed_size) {
			return {};
		}
		Layout layout{};
		layout.kind = Kind::Fixed;
		layout.size = static_cast<uint16_t>(data_size);
		return layout;
	}

	[[nodiscard]] static constexpr Layout byte_count_at(
			const std::size_t count_offset,
			const std::size_t count_width = 1u,
			const std::endian count_order = std::endian::big) noexcept
	{
		if ((count_width != 1u && count_width != 2u) ||
		    count_offset + count_width > max_header_size) {
			return {};
		}
		Layout layout{};
		layout.kind = Kind::Counted;
		layout.offset = static_cast<uint8_t>(count_offset);
		layout.width = static_cast<uint8_t>(count_width);
		layout.order = count_order;
		return layout;
	}

	[[nodiscard]] static constexpr Layout length_prefixed(
			const std::size_t count_width,
			const std::endian count_order = std::endian::big) noexcept
	{
		Layout layout = byte_count_at(0u, count_width, count_order);
		layout.owned = layout.kind == Kind::Counted;
		return layout;
	}

	[[nodiscard]] constexpr bool supported() const noexcept
	{
		return kind != Kind::Unsupported;
	}

	// Data bytes that must be present before data_size() can be answered.
	[[nodiscard]] constexpr std::size_t header_size() const noexcept
	{
		return kind == Kind::Counted
			? static_cast<std::size_t>(offset) + width
			: 0u;
	}

	// Data bytes the message builder pre-allocates and owns.
	[[nodiscard]] constexpr std::size_t reserved() const noexcept
	{
		return owned ? width : 0u;
	}

	// The complete function-data size, given at least header_size() bytes of
	// it. Unsupported answers 0; callers test supported() first.
	[[nodiscard]] constexpr std::size_t data_size(
			const std::span<const uint8_t> header) const noexcept
	{
		switch (kind) {
		case Kind::Fixed:
			return size;
		case Kind::Counted:
			return header_size() + load(header.data() + offset);
		case Kind::Unsupported:
			break;
		}
		return 0u;
	}

	// Whether complete function data agrees with this layout.
	[[nodiscard]] constexpr bool matches(
			const std::span<const uint8_t> data) const noexcept
	{
		switch (kind) {
		case Kind::Fixed:
			return data.size() == size;
		case Kind::Counted:
			return data.size() >= header_size() && data_size(data) == data.size();
		case Kind::Unsupported:
			break;
		}
		return false;
	}

	// Counted only, data_size >= header_size(): writes the count of the bytes
	// following the field.
	constexpr void store_count(
			uint8_t* const data,
			const std::size_t data_size) const noexcept
	{
		const std::size_t count = data_size - header_size();
		if (width == 1u) {
			data[offset] = static_cast<uint8_t>(count);
			return;
		}
		const uint8_t high = static_cast<uint8_t>(count >> 8u);
		const uint8_t low = static_cast<uint8_t>(count);
		data[offset] = order == std::endian::big ? high : low;
		data[offset + 1u] = order == std::endian::big ? low : high;
	}

	[[nodiscard]] constexpr bool operator==(const Layout&) const noexcept = default;

private:
	[[nodiscard]] constexpr std::size_t load(const uint8_t* const field) const noexcept
	{
		if (width == 1u) {
			return field[0];
		}
		const std::size_t first = field[0];
		const std::size_t second = field[1];
		return order == std::endian::big ? (first << 8u) | second
		                                 : (second << 8u) | first;
	}
};

/*
 * The standard function table, Modbus Application Protocol V1.1b3 §6.
 * `function` with bit 7 set is an exception response: one exception code,
 * and there is no such thing as an exception request.
 */
[[nodiscard]] constexpr Layout standard_layout(
		const Direction direction,
		const uint8_t function) noexcept
{
	const bool request = direction == Direction::Request;
	if ((function & 0x80u) != 0u) {
		return request ? Layout::unsupported() : Layout::fixed(1u);
	}
	switch (function) {
	case 0x01u: // Read Coils
	case 0x02u: // Read Discrete Inputs
	case 0x03u: // Read Holding Registers
	case 0x04u: // Read Input Registers
		return request ? Layout::fixed(4u) : Layout::byte_count_at(0u);
	case 0x05u: // Write Single Coil
	case 0x06u: // Write Single Register
		return Layout::fixed(4u);
	case 0x07u: // Read Exception Status (serial line only)
		return request ? Layout::fixed(0u) : Layout::fixed(1u);
	case 0x0Bu: // Get Comm Event Counter (serial line only)
		return request ? Layout::fixed(0u) : Layout::fixed(4u);
	case 0x0Cu: // Get Comm Event Log (serial line only)
		return request ? Layout::fixed(0u) : Layout::byte_count_at(0u);
	case 0x0Fu: // Write Multiple Coils
	case 0x10u: // Write Multiple Registers
		return request ? Layout::byte_count_at(4u) : Layout::fixed(4u);
	case 0x11u: // Report Server ID (serial line only)
		return request ? Layout::fixed(0u) : Layout::byte_count_at(0u);
	case 0x14u: // Read File Record
	case 0x15u: // Write File Record
		return Layout::byte_count_at(0u);
	case 0x16u: // Mask Write Register
		return Layout::fixed(6u);
	case 0x17u: // Read/Write Multiple Registers
		return request ? Layout::byte_count_at(8u) : Layout::byte_count_at(0u);
	case 0x18u: // Read FIFO Queue: the response byte count is two bytes wide
		return request ? Layout::fixed(2u) : Layout::byte_count_at(0u, 2u);
	// 0x08 Diagnostics and 0x2B Encapsulated Interface Transport carry no
	// length indicator covering their data.
	default:
		return Layout::unsupported();
	}
}

// The default: no framing. receive_adu() only, every function code accepted.
struct None final {};

// A framing policy: which direction it receives, and a layout per function.
template<class F>
concept Policy = requires(const Direction direction, const uint8_t function) {
	{ F::rx } -> std::convertible_to<Direction>;
	{ F::layout(direction, function) } noexcept -> std::same_as<Layout>;
};

template<class F>
concept Framer = std::same_as<F, None> || Policy<F>;

template<Direction Rx>
struct Standard {
	static constexpr Direction rx = Rx;

	[[nodiscard]] static constexpr Layout layout(
			const Direction direction,
			const uint8_t function) noexcept
	{
		return standard_layout(direction, function);
	}
};

static_assert(Policy<Standard<Direction::Request>>);
static_assert(Policy<Standard<Direction::Response>>);
static_assert(Framer<None> && !Policy<None>);

} // namespace modbus::rtu::framing

#endif /* MODBUS_RTU_FRAMING_H_ */
