/*
 * Host verification for cobs/cobs::codec::Decoder.
 *
 * The decoder holds no allocator and no transport, so this whole battery is a
 * plain host binary: bytes in, bytes out, no HAL anywhere.
 *
 * The encoder used here is the shared test reference (reference_encoder.h),
 * canonical and straightforward. The production in-place encoder
 * (doc/COBS_ENGINE.md §8.3) is a different piece of code and gets its own
 * tests when it exists.
 */
#include "Codec.h"
#include "reference_encoder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdint>
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

// Reported only on failure: a battery of thousands of property cases must not
// print thousands of lines when it is green.
void expect(const bool ok, const std::string& what)
{
	++g_checks;
	if (!ok) {
		++g_failures;
		std::printf("  FAIL  %s\n", what.c_str());
	}
}

using cobs_test::encode;

/* ---------------------------- drive helper ------------------------------ */

struct Decoded {
	std::vector<std::vector<uint8_t>> frames;
	int malformed = 0;
	int refused = 0;      // owner refused to grow: the frame did not fit
	int need_output = 0;
	int alloc_denied = 0;
};

// Feeds the decoder in slices, acting as the owner would: answering
// NeedOutput from a scratch buffer and collecting completed frames.
// `deny_first` refuses allocation for that many frames, then behaves normally.
Decoded drive(cobs::codec::Decoder& dec, const std::vector<uint8_t>& wire,
              const std::vector<std::size_t>& cuts, const std::size_t capacity,
              const int deny_first = 0)
{
	Decoded r;
	static std::vector<uint8_t> scratch;
	scratch.assign(capacity, 0xCC);
	bool attached = false;

	std::size_t pos = 0;
	for (std::size_t c = 0; c <= cuts.size(); ++c) {
		const std::size_t end = (c < cuts.size()) ? cuts[c] : wire.size();
		while (pos < end) {
			const std::span<const uint8_t> in{wire.data() + pos, end - pos};
			const auto res = dec.consume(in);
			pos += res.consumed;
			switch (res.event) {
			case cobs::codec::Decoder::Event::None:
				break;
			case cobs::codec::Decoder::Event::NeedOutput:
				++r.need_output;
				if (r.need_output <= deny_first) {
					++r.alloc_denied;
					dec.discard_until_delimiter();
					attached = false;
				} else if (attached) {
					// A SECOND request inside one frame: the segment filled and
					// the decoder wants more. This owner has only one buffer,
					// so the frame does not fit — which is the owner's verdict
					// to make now, not the decoder's.
					++r.refused;
					dec.discard_until_delimiter();
					attached = false;
				} else {
					dec.attach_output(std::span<uint8_t>{scratch});
					attached = true;
				}
				break;
			case cobs::codec::Decoder::Event::FrameComplete:
				r.frames.emplace_back(scratch.begin(),
				                      scratch.begin() + static_cast<std::ptrdiff_t>(res.decoded_size));
				attached = false;
				break;
			case cobs::codec::Decoder::Event::Malformed:
				++r.malformed;
				attached = false;
				break;
			}
			if (res.consumed == 0 && res.event == cobs::codec::Decoder::Event::None) {
				break; // defensive: never spin
			}
		}
	}
	return r;
}

Decoded driveWhole(cobs::codec::Decoder& dec, const std::vector<uint8_t>& wire,
                   const std::size_t capacity)
{
	return drive(dec, wire, {}, capacity);
}

std::string hex(const std::vector<uint8_t>& v)
{
	std::string s;
	char buf[8];
	for (const uint8_t b : v) {
		std::snprintf(buf, sizeof buf, "%02X ", b);
		s += buf;
	}
	return s;
}

/* ============================ wire semantics ============================= */

void testWireSemantics()
{
	const std::size_t cap = 512;

	{	// The empty packet is a real packet.
		cobs::codec::Decoder d;
		const auto r = driveWhole(d, {0x01, 0x00}, cap);
		check(r.frames.size() == 1 && r.frames[0].empty(),
		      "01 00 delivers one empty packet");
		check(r.need_output == 1, "and it allocates, like any other frame");
	}
	{	// A bare delimiter is not a packet.
		cobs::codec::Decoder d;
		const auto r = driveWhole(d, {0x00, 0x00, 0x00, 0x00}, cap);
		check(r.frames.empty() && r.need_output == 0,
		      "a run of bare delimiters delivers nothing and allocates nothing");
	}
	{	// A leading delimiter must be free.
		cobs::codec::Decoder d;
		const auto r = driveWhole(d, {0x00, 0x03, 0x11, 0x22, 0x00}, cap);
		check(r.frames.size() == 1 && r.frames[0] == std::vector<uint8_t>({0x11, 0x22}),
		      "a leading delimiter is harmless");
	}
	{	// The implicit zero, materialized only when the frame continues.
		cobs::codec::Decoder d;
		const auto r = driveWhole(d, {0x01, 0x01, 0x00}, cap);
		check(r.frames.size() == 1 && r.frames[0] == std::vector<uint8_t>({0x00}),
		      "01 01 00 decodes to a single zero byte");
	}
	{	// 0xFF carries no implicit zero.
		std::vector<uint8_t> wire{0xFF};
		for (int i = 0; i < 254; ++i) { wire.push_back(0x41); }
		wire.push_back(0x00);
		cobs::codec::Decoder d;
		const auto r = driveWhole(d, wire, cap);
		check(r.frames.size() == 1 && r.frames[0].size() == 254,
		      "an FF block yields 254 bytes with no trailing zero");
	}
	{	// Structurally valid but non-canonical input must still decode.
		std::vector<uint8_t> wire{0xFF};
		for (int i = 0; i < 254; ++i) { wire.push_back(0x41); }
		wire.push_back(0x01); // redundant empty block
		wire.push_back(0x00);
		cobs::codec::Decoder d;
		const auto r = driveWhole(d, wire, cap);
		check(r.frames.size() == 1 && r.frames[0].size() == 254,
		      "a redundant-but-valid trailing block decodes identically");
	}
	{	// Several frames in one span.
		cobs::codec::Decoder d;
		const auto r = driveWhole(d, {0x03, 0x11, 0x22, 0x00, 0x02, 0xAA, 0x00, 0x01, 0x00}, cap);
		check(r.frames.size() == 3, "three frames in one span are all delivered");
		check(r.frames[0] == std::vector<uint8_t>({0x11, 0x22}) &&
		      r.frames[1] == std::vector<uint8_t>({0xAA}) &&
		      r.frames[2].empty(), "and in the right order, with the right contents");
	}
}

/* ========================== the NeedOutput seam ========================== */

void testNeedOutputSeam()
{
	{	// The code byte that starts a frame is consumed exactly once.
		cobs::codec::Decoder d;
		const std::vector<uint8_t> wire{0x03, 0x11, 0x22, 0x00};
		const auto r1 = d.consume(std::span<const uint8_t>{wire});
		check(r1.event == cobs::codec::Decoder::Event::NeedOutput, "a frame start asks for output");
		check(r1.consumed == 1, "having consumed exactly the code byte");

		std::vector<uint8_t> out(64, 0xCC);
		d.attach_output(std::span<uint8_t>{out});
		const auto r2 = d.consume(std::span<const uint8_t>{wire.data() + 1, 3});
		check(r2.event == cobs::codec::Decoder::Event::FrameComplete && r2.decoded_size == 2,
		      "the remainder completes the frame without re-feeding the code byte");
	}
	{ // A known first destination can be prepared before the frame starts.
		cobs::codec::Decoder d;
		std::array<uint8_t, 2> out{0xCC, 0xCC};
		d.prepare_output(out);
		const std::array<uint8_t, 5> wire{0x00, 0x03, 0x11, 0x22, 0x00};
		const auto r = d.consume(wire);
		check(r.event == cobs::codec::Decoder::Event::FrameComplete &&
		      r.consumed == wire.size() && r.decoded_size == out.size(),
		      "a prepared first segment avoids the initial NeedOutput round trip");
		check(out == std::array<uint8_t, 2>{0x11, 0x22},
		      "and remains prepared across harmless leading delimiters");
	}
	{	// Ignoring NeedOutput must not corrupt anything: the decoder asks again.
		cobs::codec::Decoder d;
		const std::vector<uint8_t> wire{0x03, 0x11, 0x22, 0x00};
		(void)d.consume(std::span<const uint8_t>{wire});
		const auto r = d.consume(std::span<const uint8_t>{wire.data() + 1, 3});
		check(r.event == cobs::codec::Decoder::Event::NeedOutput && r.consumed == 0,
		      "an unanswered NeedOutput is repeated, writing nowhere");
	}
	{	// Refusing to allocate costs exactly one frame.
		cobs::codec::Decoder d;
		const std::vector<uint8_t> wire{0x03, 0x11, 0x22, 0x00, 0x02, 0xAA, 0x00};
		const auto r = drive(d, wire, {}, 64, 1); // deny the very first frame
		check(r.alloc_denied == 1, "the first frame is denied");
		check(r.frames.size() == 1 && r.frames[0] == std::vector<uint8_t>({0xAA}),
		      "and the next frame after the delimiter decodes normally");
	}
}

/* ============================ error handling ============================= */

void testMalformed()
{
	const std::size_t cap = 512;
	{
		cobs::codec::Decoder d;
		// 0x04 promises three data bytes; a zero arrives as the second.
		const auto r = driveWhole(d, {0x04, 0x11, 0x00, 0x02, 0xAA, 0x00}, cap);
		check(r.malformed == 1, "a delimiter inside a block is malformed");
		check(r.frames.size() == 1 && r.frames[0] == std::vector<uint8_t>({0xAA}),
		      "and the very next frame is decoded — no second delimiter required");
	}
	{	// Every data position of a block must behave the same way.
		bool all_ok = true;
		for (int pos = 1; pos <= 3; ++pos) {
			std::vector<uint8_t> wire{0x05, 0x11, 0x22, 0x33, 0x00};
			wire[static_cast<std::size_t>(pos)] = 0x00;
			wire.resize(static_cast<std::size_t>(pos) + 1);
			for (const uint8_t b : {uint8_t{0x02}, uint8_t{0xAA}, uint8_t{0x00}}) {
				wire.push_back(b);
			}
			cobs::codec::Decoder d;
			const auto r = driveWhole(d, wire, cap);
			all_ok = all_ok && r.malformed == 1 && r.frames.size() == 1 &&
			         r.frames[0] == std::vector<uint8_t>({0xAA});
		}
		check(all_ok, "a delimiter at any data position drops the frame and resynchronizes");
	}
}

void testDoesNotFit()
{
	{	// A data byte that does not fit.
		cobs::codec::Decoder d;
		const std::vector<uint8_t> wire{0x05, 0x11, 0x22, 0x33, 0x00, 0x02, 0xAA, 0x00};
		const auto r = drive(d, wire, {}, 2); // capacity 2, frame needs 4
		check(r.refused == 1, "a frame larger than the owner's buffer is refused by the owner");
		check(r.frames.size() == 1 && r.frames[0] == std::vector<uint8_t>({0xAA}),
		      "and the next frame still decodes");
	}
	{	// Exactly full must NOT be oversize.
		cobs::codec::Decoder d;
		const auto r = drive(d, {0x04, 0x11, 0x22, 0x33, 0x00}, {}, 3);
		check(r.refused == 0 && r.frames.size() == 1 && r.frames[0].size() == 3,
		      "a frame that exactly fills the output is accepted, with no further request");
	}
	{	// One byte too small must be.
		cobs::codec::Decoder d;
		const auto r = drive(d, {0x05, 0x11, 0x22, 0x33, 0x44, 0x00}, {}, 3);
		check(r.refused == 1 && r.frames.empty(),
		      "one byte too many asks for another segment");
	}
	{	// The implicit-zero overflow. It is detected at the code byte that
		// follows the owed zero, BEFORE that byte is taken — so when a span
		// happens to begin exactly there, consumed is 0. That must not
		// livelock, because the state has already changed.
		cobs::codec::Decoder d;
		// Payload 11 22 00 33: two blocks with an implicit zero between them.
		const std::vector<uint8_t> wire{0x03, 0x11, 0x22, 0x02, 0x33, 0x00};
		std::vector<uint8_t> out(2, 0xCC); // room for 11 22, none for the zero

		const auto r1 = d.consume(std::span<const uint8_t>{wire.data(), 1});
		check(r1.event == cobs::codec::Decoder::Event::NeedOutput && r1.consumed == 1,
		      "frame starts");
		d.attach_output(std::span<uint8_t>{out});

		const auto r2 = d.consume(std::span<const uint8_t>{wire.data() + 1, 2});
		check(r2.event == cobs::codec::Decoder::Event::None && r2.consumed == 2,
		      "the two data bytes fill the output exactly");

		// This span BEGINS with the code byte whose implicit zero needs room.
		const auto r3 = d.consume(std::span<const uint8_t>{wire.data() + 3, 3});
		check(r3.event == cobs::codec::Decoder::Event::NeedOutput,
		      "the implicit zero that does not fit asks for another segment");
		check(r3.consumed == 0, "reported without consuming the code byte that triggered it");

		// Answering it must place the zero, not lose it.
		std::vector<uint8_t> more(4, 0xEE);
		d.attach_output(std::span<uint8_t>{more});
		const auto r4 = d.consume(std::span<const uint8_t>{wire.data() + 3, 3});
		check(r4.event == cobs::codec::Decoder::Event::FrameComplete && r4.decoded_size == 4,
		      "and the frame completes with all four decoded bytes");
		check(out[0] == 0x11 && out[1] == 0x22 && more[0] == 0x00 && more[1] == 0x33,
		      "the implicit zero landed at the head of the second segment");
	}
}

/* ================================ gaps ================================== */

void testGaps()
{
	const std::size_t cap = 512;
	{	// A gap while Synced still requires a delimiter before decoding.
		cobs::codec::Decoder d;
		d.discard_until_delimiter();
		const auto r = driveWhole(d, {0x03, 0x11, 0x22, 0x00, 0x02, 0xAA, 0x00}, cap);
		check(r.frames.size() == 1 && r.frames[0] == std::vector<uint8_t>({0xAA}),
		      "after a gap the first frame is discarded even if it looks well formed");
		check(r.need_output == 1, "and no output is requested for the discarded one");
	}
	{	// A gap at every byte position of a stream: never a spurious frame,
		// and always recovery at the next delimiter.
		const std::vector<uint8_t> wire{0x03, 0x11, 0x22, 0x00, 0x04, 0x33, 0x44, 0x55, 0x00};
		bool all_ok = true;
		for (std::size_t cut = 0; cut <= wire.size(); ++cut) {
			cobs::codec::Decoder d;
			const std::vector<uint8_t> head(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(cut));
			const std::vector<uint8_t> tail(wire.begin() + static_cast<std::ptrdiff_t>(cut), wire.end());
			(void)drive(d, head, {}, cap);
			d.discard_until_delimiter(); // the transport reports the loss here
			const auto r = drive(d, tail, {}, cap);
			// Whatever survives must be a frame that started after a delimiter
			// in the tail; nothing may be delivered from the damaged region.
			for (const auto& f : r.frames) {
				all_ok = all_ok && (f == std::vector<uint8_t>({0x33, 0x44, 0x55}));
			}
		}
		check(all_ok, "a gap at any position never yields a spliced or bogus frame");
	}
}

/* =========================== property battery =========================== */

std::vector<std::vector<uint8_t>> patterns(const std::size_t n)
{
	std::vector<std::vector<uint8_t>> out;
	out.emplace_back(n, 0x41);                       // all non-zero
	out.emplace_back(n, 0x00);                       // all zero
	{ std::vector<uint8_t> v(n, 0x41); if (n) { v.front() = 0; } out.push_back(v); }
	{ std::vector<uint8_t> v(n, 0x41); if (n) { v.back() = 0; }  out.push_back(v); }
	{	// a zero every 254 bytes
		std::vector<uint8_t> v(n, 0x41);
		for (std::size_t i = 253; i < n; i += 254) { v[i] = 0; }
		out.push_back(v);
	}
	{	// alternating
		std::vector<uint8_t> v(n);
		for (std::size_t i = 0; i < n; ++i) { v[i] = (i % 2 == 0) ? 0x00 : 0x5A; }
		out.push_back(v);
	}
	{	// deterministic pseudo-random, zeros included
		std::vector<uint8_t> v(n);
		uint32_t s = 0x12345678u ^ static_cast<uint32_t>(n);
		for (std::size_t i = 0; i < n; ++i) {
			s ^= s << 13; s ^= s >> 17; s ^= s << 5;
			v[i] = static_cast<uint8_t>(s);
		}
		out.push_back(v);
	}
	return out;
}

std::size_t maxEncoded(const std::size_t n)
{
	return (n == 0) ? 1u : n + (n + 253u) / 254u;
}

void testRoundTrip()
{
	const std::size_t kMax = 1024;
	const std::size_t lengths[] = {0, 1, 253, 254, 255, 508, 509, kMax};

	std::size_t cases = 0;
	bool bound_tight = true;
	for (const std::size_t n : lengths) {
		for (const auto& payload : patterns(n)) {
			const auto wire = encode(payload);
			// The bound holds, and zero-free data attains it for every length,
			// which is what makes it tight. Other payloads may attain it too:
			// below 255 bytes every payload does (COBS_ENGINE.md §4.2).
			expect(wire.size() - 1u <= maxEncoded(n),
			       "encoded length within the bound for n=" + std::to_string(n));
			const bool zero_free = (n > 0) &&
				(std::find(payload.begin(), payload.end(), 0u) == payload.end());
			if (zero_free && (wire.size() - 1u) != maxEncoded(n)) {
				bound_tight = false;
			}

			// Whole, then every possible single split, then byte by byte.
			for (std::size_t cut = 0; cut <= wire.size(); ++cut) {
				cobs::codec::Decoder d;
				const auto r = drive(d, wire, {cut}, kMax);
				expect(r.frames.size() == 1 && r.frames[0] == payload,
				       "round trip n=" + std::to_string(n) + " cut=" + std::to_string(cut) +
				       " wire=" + hex(wire).substr(0, 48));
				++cases;
			}
			{	// one byte per span
				std::vector<std::size_t> cuts;
				for (std::size_t k = 1; k < wire.size(); ++k) { cuts.push_back(k); }
				cobs::codec::Decoder d;
				const auto r = drive(d, wire, cuts, kMax);
				expect(r.frames.size() == 1 && r.frames[0] == payload,
				       "round trip byte-by-byte n=" + std::to_string(n));
				++cases;
			}
		}
	}
	check(true, "round trip over " + std::to_string(cases) +
	            " length x pattern x boundary combinations");
	check(bound_tight, "zero-free payloads attain the size bound at every length");
}

// Two frames back to back, split at every possible point: proves the decoder
// carries no state across a span boundary that a single frame would hide.
void testStreamingBoundaries()
{
	std::vector<uint8_t> payload_a(300, 0x41);
	payload_a[100] = 0;
	payload_a[254] = 0;
	const std::vector<uint8_t> payload_b{0x01, 0x00, 0x02};

	std::vector<uint8_t> wire = encode(payload_a);
	for (const uint8_t b : encode(payload_b)) { wire.push_back(b); }

	bool all_ok = true;
	for (std::size_t cut = 0; cut <= wire.size(); ++cut) {
		cobs::codec::Decoder d;
		const auto r = drive(d, wire, {cut}, 1024);
		all_ok = all_ok && r.frames.size() == 2 &&
		         r.frames[0] == payload_a && r.frames[1] == payload_b;
	}
	check(all_ok, "two frames survive a split at every one of " +
	              std::to_string(wire.size() + 1) + " positions");
}


/* ========================== segmented output ============================ */

/*
 * The decoder writes into a CHAIN of segments, asking for the next one
 * whenever the current fills. That is what lets a length-prefixed protocol
 * decode a header into two local bytes, learn the body size, allocate exactly
 * that, and continue straight into the packet — no staging buffer, no copy.
 *
 * The property that matters: the decoded bytes do not depend on where the
 * segment boundaries fall. So every case below decodes the same wire under
 * several segmentation plans and compares against the single-span answer.
 */
struct Segmented {
	std::vector<uint8_t> decoded;
	std::size_t total = 0;
	int need_output = 0;
	bool complete = false;
};

// Hands out segments of the given sizes in order; once they run out, keeps
// handing out segments of `tail` bytes.
Segmented driveSegmented(const std::vector<uint8_t>& wire,
                         const std::vector<std::size_t>& plan,
                         const std::size_t tail = 64)
{
	Segmented r;
	cobs::codec::Decoder d;
	std::vector<std::vector<uint8_t>> segments;
	std::size_t next = 0;

	std::size_t pos = 0;
	int guard = 0;
	while (pos <= wire.size() && guard++ < 100000) {
		const auto res = d.consume(
			std::span<const uint8_t>{wire.data() + pos, wire.size() - pos});
		pos += res.consumed;
		if (res.event == cobs::codec::Decoder::Event::NeedOutput) {
			++r.need_output;
			const std::size_t n = (next < plan.size()) ? plan[next] : tail;
			++next;
			segments.emplace_back(n, 0xCC);
			d.attach_output(std::span<uint8_t>{segments.back()});
			continue;
		}
		if (res.event == cobs::codec::Decoder::Event::FrameComplete) {
			r.total = res.decoded_size;
			r.complete = true;
			break;
		}
		if (res.event == cobs::codec::Decoder::Event::None) {
			break;
		}
		break; // Malformed
	}
	// Segments fill in order, so concatenating and truncating to the reported
	// total reconstructs exactly what was decoded.
	for (const auto& seg : segments) {
		r.decoded.insert(r.decoded.end(), seg.begin(), seg.end());
	}
	r.decoded.resize(r.total);
	return r;
}

void testSegmentedOutput()
{
	// A payload with implicit zeros, a 0xFF block boundary and a trailing
	// zero: every place a segment boundary can be awkward.
	std::vector<uint8_t> payload;
	for (std::size_t i = 0; i < 600; ++i) {
		payload.push_back(static_cast<uint8_t>((i % 7 == 3) ? 0 : (1 + (i % 250))));
	}
	payload.push_back(0);
	const auto wire = cobs_test::encode(payload);

	const auto whole = driveSegmented(wire, {payload.size() + 8});
	check(whole.complete && whole.decoded == payload,
	      "a single large segment decodes the whole frame");
	check(whole.need_output == 1, "asking for output exactly once");

	// Every plan must produce the identical result.
	const std::vector<std::vector<std::size_t>> plans = {
		{1},                       // one byte at a time, forever
		{2},                       // a two-byte header, then two-byte segments
		{2, payload.size()},       // header then exactly the body: the real shape
		{1, 1, 1, 3, 250, 7},      // ragged
		{253}, {254}, {255},       // around the COBS block boundary
		{payload.size() - 1},      // one byte short, then more
	};
	bool all_ok = true;
	for (const auto& plan : plans) {
		const auto r = driveSegmented(wire, plan, plan.back());
		all_ok = all_ok && r.complete && r.decoded == payload && r.total == payload.size();
	}
	check(all_ok, "and so does every one of " + std::to_string(plans.size()) +
	              " segmentation plans, byte for byte");

	{	// A segment boundary falling exactly where an implicit zero is owed:
		// the zero must land at the head of the NEXT segment, not vanish.
		const std::vector<uint8_t> p{0x11, 0x22, 0x00, 0x33};
		const auto w = cobs_test::encode(p);
		bool ok = true;
		for (std::size_t first = 1; first <= p.size(); ++first) {
			const auto r = driveSegmented(w, {first}, 1);
			ok = ok && r.complete && r.decoded == p;
		}
		check(ok, "an implicit zero survives a boundary at every position");
	}

	{	// The delimiter arriving exactly when the segment is full completes the
		// frame; it must not ask for a segment it will never use.
		const std::vector<uint8_t> p{0x11, 0x22, 0x33};
		const auto w = cobs_test::encode(p);
		const auto r = driveSegmented(w, {3}, 3);
		check(r.complete && r.decoded == p && r.need_output == 1,
		      "a frame that exactly fills its segment needs no further segment");
	}

	{	// NeedOutput for a full segment must not consume the byte that still
		// needs a home, or the stream loses a byte per boundary.
		cobs::codec::Decoder d;
		const std::vector<uint8_t> w{0x04, 0xAA, 0xBB, 0xCC, 0x00};
		std::vector<uint8_t> a(1, 0), b(4, 0);

		auto res = d.consume(std::span<const uint8_t>{w});
		check(res.event == cobs::codec::Decoder::Event::NeedOutput && res.consumed == 1,
		      "the frame starts");
		d.attach_output(std::span<uint8_t>{a});

		res = d.consume(std::span<const uint8_t>{w.data() + 1, 4});
		check(res.event == cobs::codec::Decoder::Event::NeedOutput && res.consumed == 1,
		      "one byte fits, then the segment is full");
		check(a[0] == 0xAA, "with that byte written");
		d.attach_output(std::span<uint8_t>{b});

		res = d.consume(std::span<const uint8_t>{w.data() + 2, 3});
		check(res.event == cobs::codec::Decoder::Event::FrameComplete && res.decoded_size == 3,
		      "and the rest lands in the next segment");
		check(b[0] == 0xBB && b[1] == 0xCC, "with nothing lost at the boundary");
	}

	{	// Attaching over a segment that still has room would strand the bytes
		// already in it, so it is ignored.
		cobs::codec::Decoder d;
		const std::vector<uint8_t> w{0x03, 0xAA, 0xBB, 0x00};
		std::vector<uint8_t> a(8, 0), b(8, 0);
		(void)d.consume(std::span<const uint8_t>{w.data(), 1});
		d.attach_output(std::span<uint8_t>{a});
		(void)d.consume(std::span<const uint8_t>{w.data() + 1, 1});
		d.attach_output(std::span<uint8_t>{b}); // refused: `a` is not full
		const auto res = d.consume(std::span<const uint8_t>{w.data() + 2, 2});
		check(res.event == cobs::codec::Decoder::Event::FrameComplete && res.decoded_size == 2,
		      "attaching over a segment with room left is ignored");
		check(a[0] == 0xAA && a[1] == 0xBB && b[0] == 0,
		      "and the bytes stay where they were going");
	}
}

} // namespace

int main()
{
	group("WireSemantics");
	testWireSemantics();

	group("NeedOutputSeam");
	testNeedOutputSeam();

	group("Malformed");
	testMalformed();

	group("DoesNotFit");
	testDoesNotFit();

	group("SegmentedOutput");
	testSegmentedOutput();

	group("Gaps");
	testGaps();

	group("RoundTrip");
	testRoundTrip();

	group("StreamingBoundaries");
	testStreamingBoundaries();

	std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
