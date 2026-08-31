/*
 * CobsEncoder — canonical COBS encoding, in place, over the payload itself.
 *
 * Contract: doc/COBS_ENGINE.md §3, §4.2 and §8.3–§8.4. Like the decoder this
 * is a free function over a caller-supplied span: no allocator, no transport,
 * no ownership, one compilation regardless of how many CobsMsg types exist.
 *
 * Layout (§8.3): the raw payload sits at an offset inside the storage, and
 * the encoder writes the wire frame forward from the start, reading the
 * payload ahead of itself.
 *
 *      storage
 *      |<-- raw_offset -->|<------- raw_size ------->|
 *      +------------------+--------------------------+---+
 *      | headroom         | raw payload              |   |
 *      +------------------+--------------------------+---+
 *      ^ the wire frame is written from here
 *
 * The overlap invariant, proved in §8.4:
 *
 *      written <= consumed + raw_offset - 1     while raw bytes remain
 *
 * and max(written - consumed) = ceil(N / 254) exactly, so a raw_offset of
 * cobs_raw_offset(N) = ceil(N / 254) + 1 leaves precisely one byte of
 * separation. That "+1" is load-bearing, not decorative: without it the
 * writer lands on the very byte it is about to read.
 */

#ifndef COBS_ENCODER_H_
#define COBS_ENCODER_H_

#include <cstddef>
#include <cstdint>
#include <span>

// Longest COBS encoding of n payload bytes, delimiter NOT included.
// A tight upper bound, not an exact length (§4.2): zero-free payloads attain
// it at every n, and below 255 bytes every payload does.
[[nodiscard]] constexpr std::size_t cobs_max_encoded_size(const std::size_t n) noexcept
{
	return (n == 0u) ? 1u : n + (n + 253u) / 254u;
}

// ...including the delimiter. This is also the exact size of a TX block able
// to hold any frame of up to n payload bytes.
[[nodiscard]] constexpr std::size_t cobs_max_wire_size(const std::size_t n) noexcept
{
	return cobs_max_encoded_size(n) + 1u;
}

// Where the raw payload must start inside such a block. Everything before it
// is the headroom the encoder writes into while reading ahead.
[[nodiscard]] constexpr std::size_t cobs_raw_offset(const std::size_t max_decoded) noexcept
{
	return cobs_max_wire_size(max_decoded) - max_decoded;
}

/*
 * Whether the functions above are meaningful for `n` at all.
 *
 * They add COBS overhead to a std::size_t, so a pathological limit wraps: at
 * n = SIZE_MAX, cobs_max_wire_size() returns 0 and cobs_raw_offset() returns
 * 1, which is below the minimum headroom the encoder needs. A policy built on
 * that would ask its allocator for a block SMALLER than the payload it
 * intends to put in it — silently, and only for a configuration nobody would
 * write on purpose.
 *
 * Rather than leave a correctness hole priced at "surely nobody would", the
 * policies static_assert this, so such a configuration does not compile.
 *
 * The arithmetic is deliberately different from the functions it guards: it
 * must not itself overflow, so the overhead is computed without ever forming
 * n + 253.
 */
[[nodiscard]] constexpr bool cobs_size_arithmetic_fits(const std::size_t n) noexcept
{
	if (n == 0u) {
		return true;
	}
	const std::size_t overhead = n / 254u + ((n % 254u != 0u) ? 1u : 0u) + 1u;
	return n <= static_cast<std::size_t>(-1) - overhead;
}

/*
 * Encodes storage[raw_offset .. raw_offset + raw_size) in place and returns
 * the finished wire frame, starting at storage[0] and including the trailing
 * delimiter.
 *
 * Returns an EMPTY span if the layout cannot be encoded safely — the payload
 * does not fit the storage, the storage cannot hold the worst-case frame, or
 * the headroom is too small for the overlap invariant. An empty return is
 * unambiguous: the shortest possible frame is `01 00`, two bytes.
 *
 * The encoding is canonical: a payload that is an exact multiple of 254
 * non-zero bytes ends on its 0xFF block with no redundant trailing `01`,
 * while a payload ending in a zero does get the final `01` that materializes
 * it. Those two cases look alike and are not.
 */
[[nodiscard]] std::span<const uint8_t> cobs_encode_in_place(
	std::span<uint8_t> storage,
	std::size_t raw_offset,
	std::size_t raw_size) noexcept;

#endif /* COBS_ENCODER_H_ */
