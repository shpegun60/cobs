/*
 * cobs::Format — protocol geometry, independent of memory strategy.
 *
 * Contract: doc/PROTOCOL.md. Every engine frame carries a fixed-width decoded
 * length prefix ahead of its body:
 *
 *     wire     =  COBS( [length][body] ) 00
 *     length   =  the number of decoded BODY bytes that follow it
 *
 * The length never counts itself. In v1 the body IS the application payload;
 * a future integrity trailer would live inside the body and would therefore
 * be included in the declared length, which is the whole point of defining it
 * this way now rather than after the trailer exists.
 *
 * WHY A LENGTH AT ALL. Without it, RX has to commit to rx_max_size before the
 * first payload byte arrives, because a COBS frame announces its size only by
 * ending. With it, the decoder can be given the length prefix first, and the
 * packet can then be allocated at exactly the size that is coming — still with
 * no staging buffer and no copy, because the body decodes straight into its
 * final home.
 *
 * ---------------------------------------------------------------------------
 * WIRE FORMAT PROPERTY, NOT A MEMORY PROPERTY. The width is chosen from the
 * LARGER of the two directional limits:
 *
 *     length_size = max(rx_max_size, tx_max_size) <= 255 ? 1 : 2
 *
 * so that one engine speaks ONE header width in both directions. Choosing it
 * per direction would let an engine with rx_max_size = 1024 and
 * tx_max_size = 64 expect two bytes and send one, which is a wire-format
 * disagreement with itself.
 *
 * This does NOT merge the limits. With RX = 1024 and TX = 64 the header is two
 * bytes both ways, RX still refuses frames above 1024, and TX still refuses
 * payloads above 64.
 *
 * PEERS MUST AGREE ON length_size. It is part of the wire format exactly like
 * byte order, and two peers that disagree cannot exchange even a one-byte
 * frame. A complementary pair agrees automatically:
 *
 *     Peer A: RX 1024, TX 64    -> length_size 2
 *     Peer B: RX 64,   TX 1024  -> length_size 2
 *
 * `cobs::Endpoint<A>::length_size` is constexpr so an integration build can
 * static_assert the format it expects.
 * ---------------------------------------------------------------------------
 */

#ifndef COBS_FORMAT_H_
#define COBS_FORMAT_H_

#include "Codec.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace cobs {

template<std::size_t RxMaxSize, std::size_t TxMaxSize>
struct Format final {
	// A 32-bit format is deliberately not offered. Nothing in this stack wants
	// 64 KiB frames, and an unused third width is a third thing to get wrong.
	static_assert(RxMaxSize <= UINT16_MAX,
		"rx_max_size must fit the wire length field: at most 65535");
	static_assert(TxMaxSize <= UINT16_MAX,
		"tx_max_size must fit the wire length field: at most 65535");

	static constexpr std::size_t max_receive_size = RxMaxSize;
	static constexpr std::size_t max_send_size = TxMaxSize;

	// The larger limit picks the width — see the header comment. Both
	// directions then use it.
	static constexpr std::size_t wire_length_limit =
		(RxMaxSize > TxMaxSize) ? RxMaxSize : TxMaxSize;

	static constexpr std::size_t length_size = (wire_length_limit <= UINT8_MAX) ? 1u : 2u;

	using LengthType = std::conditional_t<length_size == 1u, uint8_t, uint16_t>;

	/*
	 * Explicit little-endian, byte by byte. Never a memcpy of a LengthType:
	 * that would put this host's byte order on the wire and work perfectly
	 * until the first big-endian peer, which is the kind of bug that is found
	 * by a customer rather than by a test.
	 */
	static constexpr void store_length(uint8_t* const dst, const std::size_t n) noexcept
	{
		dst[0] = static_cast<uint8_t>(n & 0xFFu);
		if constexpr (length_size == 2u) {
			dst[1] = static_cast<uint8_t>((n >> 8) & 0xFFu);
		}
	}

	[[nodiscard]] static constexpr std::size_t load_length(const uint8_t* const src) noexcept
	{
		std::size_t n = src[0];
		if constexpr (length_size == 2u) {
			n |= static_cast<std::size_t>(src[1]) << 8;
		}
		return n;
	}

	// The decoded frame is the header plus the body, and every piece of
	// geometry below is expressed in that total rather than in the payload.
	[[nodiscard]] static constexpr std::size_t decoded_size_for_payload(
		const std::size_t payload_size) noexcept
	{
		return length_size + payload_size;
	}

	// What a TX block must physically hold to carry `capacity` payload bytes.
	[[nodiscard]] static constexpr std::size_t tx_storage_size_for_capacity(
		const std::size_t capacity) noexcept
	{
		return cobs::codec::max_wire_size(decoded_size_for_payload(capacity));
	}

	// Where the LENGTH FIELD sits inside such a block; the payload begins
	// length_size bytes later.
	[[nodiscard]] static constexpr std::size_t raw_offset_for_capacity(
		const std::size_t capacity) noexcept
	{
		return cobs::codec::raw_offset(decoded_size_for_payload(capacity));
	}

	// The guard of §4.2 still applies, now to the header-inclusive size.
	static_assert(cobs::codec::size_arithmetic_fits(decoded_size_for_payload(RxMaxSize)),
		"rx_max_size plus the length header overflows the COBS size arithmetic");
	static_assert(cobs::codec::size_arithmetic_fits(decoded_size_for_payload(TxMaxSize)),
		"tx_max_size plus the length header overflows the COBS size arithmetic");
};

} // namespace cobs

#endif /* COBS_FORMAT_H_ */
