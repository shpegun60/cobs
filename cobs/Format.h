/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Wire: COBS(length_le | payload | CRC(payload)) | 00.
 * Length counts the body (payload + trailer), never itself. Its width is
 * chosen from the larger directional body limit, so a link uses one width
 * in both directions. Peers must agree on width and integrity semantics:
 * there is no version marker or checksum autodetection.
 *
 * Layout contains only numeric geometry. Format adds the calculator type.
 * Equal-width Bitwise/Table policies share Layout, Message, Packet and the
 * bound Storage; an endpoint alone owns and invokes the calculator.
 */
#ifndef COBS_FORMAT_H_
#define COBS_FORMAT_H_

#include "Codec.h"
#include "../crc/Crc.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cobs {

template<std::size_t CrcSize, std::size_t RxMaxSize, std::size_t TxMaxSize = RxMaxSize>
struct Layout {
	static_assert(CrcSize <= UINT16_MAX,
		"CRC trailer must fit the COBS 16-bit body length");
	static_assert(RxMaxSize <= UINT16_MAX - CrcSize,
		"RX payload plus CRC must fit the COBS 16-bit body length");
	static_assert(TxMaxSize <= UINT16_MAX - CrcSize,
		"TX payload plus CRC must fit the COBS 16-bit body length");

	static constexpr std::size_t crc_size = CrcSize;
	static constexpr std::size_t max_receive_size = RxMaxSize;
	static constexpr std::size_t max_send_size = TxMaxSize;
	static constexpr std::size_t max_receive_body = RxMaxSize + crc_size;
	static constexpr std::size_t max_send_body = TxMaxSize + crc_size;
	static constexpr std::size_t wire_length_limit =
		max_receive_body > max_send_body ? max_receive_body : max_send_body;
	static constexpr std::size_t length_size = wire_length_limit <= UINT8_MAX ? 1u : 2u;
	using LengthType = std::conditional_t<length_size == 1u, uint8_t, uint16_t>;

	static constexpr void store_length(uint8_t* dst, std::size_t body_size) noexcept
	{
		dst[0] = static_cast<uint8_t>(body_size & 0xFFu);
		if constexpr (length_size == 2u) {
			dst[1] = static_cast<uint8_t>((body_size >> 8u) & 0xFFu);
		}
	}

	[[nodiscard]] static constexpr std::size_t load_length(const uint8_t* src) noexcept
	{
		std::size_t n = src[0];
		if constexpr (length_size == 2u) {
			n |= static_cast<std::size_t>(src[1]) << 8u;
		}
		return n;
	}

	// The caller supplies a legal payload/capacity (<= its directional limit).
	[[nodiscard]] static constexpr std::size_t decoded_size_for_payload(std::size_t n) noexcept
	{
		return length_size + n + crc_size;
	}

	[[nodiscard]] static constexpr std::size_t tx_storage_size_for_capacity(std::size_t n) noexcept
	{
		return codec::max_wire_size(decoded_size_for_payload(n));
	}

	[[nodiscard]] static constexpr std::size_t raw_offset_for_capacity(std::size_t n) noexcept
	{
		return codec::raw_offset(decoded_size_for_payload(n));
	}

	// Exact O(1) inverse: M encoded bytes hold at most M - ceil(M/255)
	// decoded bytes. Subtract both header and trailer for useful capacity.
	// Clamp the physical grant first; even SIZE_MAX is safe. Used once per
	// allocation/growth, never on the append hot path.
	[[nodiscard]] static constexpr std::size_t payload_capacity_for_storage(std::size_t bytes) noexcept
	{
		if (bytes < tx_storage_size_for_capacity(0u)) { return 0u; }
		constexpr auto maximum = tx_storage_size_for_capacity(max_send_size);
		const auto encoded = (bytes < maximum ? bytes : maximum) - 1u;
		const auto codes = encoded / 255u + (encoded % 255u != 0u ? 1u : 0u);
		return encoded - codes - length_size - crc_size;
	}

	static_assert(codec::size_arithmetic_fits(decoded_size_for_payload(RxMaxSize)),
		"RX decoded size overflows COBS size arithmetic");
	static_assert(codec::size_arithmetic_fits(decoded_size_for_payload(TxMaxSize)),
		"TX decoded size overflows COBS size arithmetic");
};

namespace detail {
template<class CrcT>
consteval std::size_t default_payload_size()
{
	static_assert(CrcT::wire_size <= UINT8_MAX,
		"a CRC trailer above 255 bytes requires explicit COBS payload limits");
	return CrcT::wire_size <= UINT8_MAX ? UINT8_MAX - CrcT::wire_size : 0u;
}
} // namespace detail

template<class CrcT = ::crc::Crc16Bitwise,
         std::size_t RxMaxSize = detail::default_payload_size<CrcT>(),
         std::size_t TxMaxSize = RxMaxSize>
struct Format final : Layout<CrcT::wire_size, RxMaxSize, TxMaxSize> {
	static_assert(::crc::Policy<CrcT>, "COBS Format CRC must satisfy crc::Policy");
	using Crc = CrcT;
	using Layout = cobs::Layout<CrcT::wire_size, RxMaxSize, TxMaxSize>;
};

static_assert(Format<>::max_receive_size == 253u && Format<>::length_size == 1u);
static_assert(Format<::crc::NoCrc>::max_receive_size == 255u);
static_assert(std::is_same_v<Format<>::Layout, Format<::crc::Crc16Table>::Layout>);

} // namespace cobs
#endif /* COBS_FORMAT_H_ */
