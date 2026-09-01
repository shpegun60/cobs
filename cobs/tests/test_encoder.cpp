/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Host verification for cobs::codec::encode_in_place.
 *
 * The encoder overlaps its own input, so most of this battery is about the
 * geometry rather than the framing: every case runs with the MINIMUM legal
 * headroom (cobs::codec::raw_offset), which is where the overlap invariant has its
 * single byte of margin, and every case is surrounded by sentinel bytes.
 * An overlapping encoder tested only on round numbers has a remarkable talent
 * for working until the first 254-byte customer packet.
 *
 * Two independent oracles are used, because an encoder checked only against
 * its own decoder can share a misunderstanding with it:
 *   - the canonical reference encoder (byte-for-byte comparison), and
 *   - cobs::codec::Decoder (round trip).
 */
#include "Codec.h"
#include "reference_encoder.h"

#include <cstdio>
#include <string>
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

void expect(const bool ok, const std::string& what)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("  FAIL  %s\n", what.c_str());
	}
}

std::string hex(const std::vector<uint8_t>& v, const std::size_t limit = 16)
{
	std::string s;
	char buf[8];
	for (std::size_t i = 0; i < v.size() && i < limit; ++i) {
		std::snprintf(buf, sizeof buf, "%02X ", v[i]);
		s += buf;
	}
	if (v.size() > limit) { s += "..."; }
	return s;
}

constexpr uint8_t kSentinel = 0xE7;

// Encodes with the given headroom in a sentinel-guarded buffer, and reports
// both the frame and whether anything outside the block was touched.
struct Encoded {
	std::vector<uint8_t> wire;
	bool guards_intact = true;
	bool refused = false;
};

Encoded encodeInPlace(const std::vector<uint8_t>& payload, const std::size_t raw_offset)
{
	constexpr std::size_t kGuard = 16;
	const std::size_t block = raw_offset + payload.size();

	std::vector<uint8_t> arena(kGuard + block + kGuard, kSentinel);
	for (std::size_t i = 0; i < payload.size(); ++i) {
		arena[kGuard + raw_offset + i] = payload[i];
	}

	const std::span<uint8_t> storage{arena.data() + kGuard, block};
	const auto frame = cobs::codec::encode_in_place(storage, raw_offset, payload.size());

	Encoded r;
	r.refused = frame.empty();
	r.wire.assign(frame.begin(), frame.end());
	for (std::size_t i = 0; i < kGuard; ++i) {
		r.guards_intact = r.guards_intact && arena[i] == kSentinel &&
		                  arena[arena.size() - 1 - i] == kSentinel;
	}
	return r;
}

// Decodes a wire frame with cobs::codec::Decoder — the independent round-trip oracle.
std::vector<uint8_t> decodeFrame(const std::vector<uint8_t>& wire, bool& ok)
{
	cobs::codec::Decoder d;
	// A decoded COBS frame cannot be larger than its delimiter-inclusive wire
	// representation. Size from the case itself rather than hide a 4096-byte
	// ceiling in an oracle that is also used for the 65537-byte format maximum.
	std::vector<uint8_t> out(wire.size(), 0);
	std::vector<uint8_t> result;
	std::size_t pos = 0;
	ok = false;

	while (pos < wire.size()) {
		const auto r = d.consume(std::span<const uint8_t>{wire.data() + pos, wire.size() - pos});
		pos += r.consumed;
		switch (r.event) {
		case cobs::codec::Decoder::Event::NeedOutput:
			d.attach_output(std::span<uint8_t>{out});
			break;
		case cobs::codec::Decoder::Event::FrameComplete:
			result.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(r.decoded_size));
			ok = true;
			break;
		case cobs::codec::Decoder::Event::None:
			return result;
		default:
			return result;
		}
	}
	return result;
}

/* ========================= the two canonical traps ====================== */

void testCanonicalEndings()
{
	{	// 254 non-zero bytes end exactly on a full 0xFF block, which owes no
		// implicit zero: no trailing 01.
		const std::vector<uint8_t> p(254, 0x41);
		const auto e = encodeInPlace(p, cobs::codec::raw_offset(p.size()));
		check(e.wire.size() == 256, "254 non-zero bytes encode to FF + 254 + delimiter");
		check(e.wire.front() == 0xFF, "starting with the full block");
		check(e.wire[255] == 0x00 && e.wire[254] == 0x41,
		      "and ending at the delimiter with NO redundant trailing 01");
	}
	{	// A payload ending in a zero needs that final 01 — it is what
		// materializes the zero on decode. Looks the same, is not.
		const std::vector<uint8_t> p{0x11, 0x00};
		const auto e = encodeInPlace(p, cobs::codec::raw_offset(p.size()));
		const std::vector<uint8_t> expected{0x02, 0x11, 0x01, 0x00};
		check(e.wire == expected,
		      "a payload ending in 00 DOES get the final 01 (got " + hex(e.wire) + ")");

		bool ok = false;
		check(decodeFrame(e.wire, ok) == p && ok, "and it round-trips to the same two bytes");
	}
	{	// 254 non-zero bytes followed by a zero: the run is too long for one
		// normal block, so FF, then 01 for the zero, then the final 01.
		std::vector<uint8_t> p(254, 0x41);
		p.push_back(0x00);
		const auto e = encodeInPlace(p, cobs::codec::raw_offset(p.size()));
		check(e.wire.size() == 258 && e.wire[0] == 0xFF &&
		      e.wire[255] == 0x01 && e.wire[256] == 0x01 && e.wire[257] == 0x00,
		      "254 non-zeros then a zero: FF block, 01 for the zero, final 01");
		bool ok = false;
		check(decodeFrame(e.wire, ok) == p && ok, "and round-trips");
	}
	{	// The empty packet.
		const auto e = encodeInPlace({}, cobs::codec::raw_offset(0));
		check(e.wire == std::vector<uint8_t>({0x01, 0x00}), "the empty payload encodes to 01 00");
	}
}

/* ============================ layout refusal ============================ */

void testInvalidLayoutsAreRefused()
{
	const std::vector<uint8_t> p(300, 0x41);
	const std::size_t need = cobs::codec::raw_offset(p.size());

	{	// One byte less headroom than the invariant requires.
		const auto e = encodeInPlace(p, need - 1);
		check(e.refused, "headroom one byte below the minimum is refused");
		check(e.guards_intact, "and nothing was written outside the block");
	}
	{	// Exactly the minimum must be accepted — the refusal above must be
		// about the invariant, not about being conservative.
		const auto e = encodeInPlace(p, need);
		check(!e.refused, "exactly the minimum headroom is accepted");
	}
	{	// A payload that does not fit its own storage.
		std::vector<uint8_t> arena(64, 0);
		const auto frame = cobs::codec::encode_in_place(std::span<uint8_t>{arena}, 32, 64);
		check(frame.empty(), "a payload running past the storage is refused");
	}
	{	// Storage one byte short of the worst-case frame. Two payload bytes
		// need code + 2 + delimiter = 4, so three bytes cannot work...
		std::vector<uint8_t> small(3, 0);
		check(cobs::codec::encode_in_place(std::span<uint8_t>{small}, 1, 2).empty(),
		      "storage one byte short of the worst case is refused");

		// ...and exactly four must, or the refusal above would just be
		// timidity rather than arithmetic.
		std::vector<uint8_t> exact(4, 0);
		exact[2] = 0x11;
		exact[3] = 0x22;
		const auto frame = cobs::codec::encode_in_place(std::span<uint8_t>{exact}, 2, 2);
		check(frame.size() == 4 && frame[0] == 0x03 && frame[3] == 0x00,
		      "a storage of exactly cobs::codec::max_wire_size() is accepted");
	}
	{	// A zero-length storage cannot even hold 01 00.
		const auto frame = cobs::codec::encode_in_place(std::span<uint8_t>{}, 0, 0);
		check(frame.empty(), "empty storage is refused");
	}
}

/* =========================== property battery =========================== */

std::vector<std::vector<uint8_t>> patterns(const std::size_t n)
{
	std::vector<std::vector<uint8_t>> out;
	out.emplace_back(n, 0x41);
	out.emplace_back(n, 0x00);
	{ std::vector<uint8_t> v(n, 0x41); if (n) { v.front() = 0; } out.push_back(v); }
	{ std::vector<uint8_t> v(n, 0x41); if (n) { v.back() = 0; }  out.push_back(v); }
	{
		std::vector<uint8_t> v(n, 0x41);
		for (std::size_t i = 253; i < n; i += 254) { v[i] = 0; }
		out.push_back(v);
	}
	{
		std::vector<uint8_t> v(n);
		for (std::size_t i = 0; i < n; ++i) { v[i] = (i % 2 == 0) ? 0x00 : 0x5A; }
		out.push_back(v);
	}
	{
		std::vector<uint8_t> v(n);
		uint32_t s = 0x9E3779B9u ^ static_cast<uint32_t>(n);
		for (std::size_t i = 0; i < n; ++i) {
			s ^= s << 13; s ^= s >> 17; s ^= s << 5;
			v[i] = static_cast<uint8_t>(s);
		}
		out.push_back(v);
	}
	return out;
}

void testAgainstBothOracles()
{
	// The named lengths, and every length adjacent to them: an off-by-one in
	// the block arithmetic lives exactly one byte away from a round number.
	std::vector<std::size_t> lengths;
	for (const std::size_t n : {
		0u, 1u, 253u, 254u, 255u, 508u, 509u, 762u, 1024u, 65537u}) {
		for (int d = -2; d <= 2; ++d) {
			const long long v = static_cast<long long>(n) + d;
			if (v >= 0) { lengths.push_back(static_cast<std::size_t>(v)); }
		}
	}

	std::size_t cases = 0;
	bool guards_ok = true;
	for (const std::size_t n : lengths) {
		for (const auto& p : patterns(n)) {
			const std::size_t need = cobs::codec::raw_offset(n);
			// Minimum headroom, and one byte of slack: both must work, and
			// must produce identical output.
			for (const std::size_t offset : {need, need + 1}) {
				const auto e = encodeInPlace(p, offset);
				guards_ok = guards_ok && e.guards_intact;

				expect(!e.refused, "encoded n=" + std::to_string(n) +
				                   " offset=" + std::to_string(offset));
				expect(e.wire == cobs_test::encode(p),
				       "matches the reference encoder at n=" + std::to_string(n) +
				       " (got " + hex(e.wire) + ")");
				expect(e.wire.size() - 1u <= cobs::codec::max_encoded_size(n),
				       "within the size bound at n=" + std::to_string(n));

				bool ok = false;
				expect(decodeFrame(e.wire, ok) == p && ok,
				       "round trips through cobs::codec::Decoder at n=" + std::to_string(n));
				++cases;
			}
		}
	}
	check(true, "encoded " + std::to_string(cases) +
	            " length x pattern x headroom combinations");
	check(guards_ok, "no case wrote outside its block");
}

// The payload is consumed as it is overwritten, so a corrupted read shows up
// as wrong output rather than as a crash. This checks the one thing the
// property battery cannot: that the decoded result is the payload as it was
// BEFORE encoding, on the case with the least margin.
void testOverlapDoesNotEatItsInput()
{
	bool all_ok = true;
	for (std::size_t n = 250; n <= 262; ++n) {
		std::vector<uint8_t> p(n);
		for (std::size_t i = 0; i < n; ++i) {
			p[i] = static_cast<uint8_t>(1 + (i % 255)); // no zeros: worst case
		}
		const auto e = encodeInPlace(p, cobs::codec::raw_offset(n));
		bool ok = false;
		all_ok = all_ok && e.guards_intact && decodeFrame(e.wire, ok) == p && ok;
	}
	check(all_ok, "zero-free payloads around the 254 boundary survive being "
	              "overwritten by their own encoding");
}

} // namespace

int main()
{
	group("CanonicalEndings");
	testCanonicalEndings();

	group("LayoutRefusal");
	testInvalidLayoutsAreRefused();

	group("Oracles");
	testAgainstBothOracles();
	testOverlapDoesNotEatItsInput();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
