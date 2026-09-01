/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Exhaustive differential proof for the non-template codec core.
 *
 * The ordinary decoder/encoder suites carry the readable boundary cases.
 * This one deliberately does something different: enumerate every short
 * stream/payload over alphabets chosen to exercise delimiters, short and long
 * code bytes, zeros and 0xFF, then compare the production state machine with
 * independent reference implementations. The transcript comparison is exact
 * (not a hash), and each wire stream is replayed under several input/output
 * segmentation plans.
 */

#include "Codec.h"
#include "reference_encoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

enum class RecordKind : std::uint8_t { Frame, Malformed };

struct Record {
	RecordKind kind = RecordKind::Frame;
	std::size_t size = 0;
	std::array<std::uint8_t, 32> bytes{};

	friend bool operator==(const Record&, const Record&) = default;
};

struct Transcript {
	std::size_t size = 0;
	std::array<Record, 8> records{};

	void frame(const std::span<const std::uint8_t> bytes) noexcept
	{
		if (size == records.size() || bytes.size() > records[0].bytes.size()) {
			std::abort();
		}
		Record& record = records[size++];
		record.kind = RecordKind::Frame;
		record.size = bytes.size();
		std::copy(bytes.begin(), bytes.end(), record.bytes.begin());
	}

	void malformed() noexcept
	{
		if (size == records.size()) {
			std::abort();
		}
		Record& record = records[size++];
		record.kind = RecordKind::Malformed;
		record.size = 0;
	}

	friend bool operator==(const Transcript&, const Transcript&) = default;
};

Transcript reference_decode(const std::span<const std::uint8_t> wire)
{
	Transcript transcript;
	std::array<std::uint8_t, 32> decoded{};
	std::size_t start = 0;
	for (std::size_t end = 0; end < wire.size(); ++end) {
		if (wire[end] != 0u) {
			continue;
		}
		if (end == start) {
			start = end + 1u; // a bare delimiter is not an empty frame
			continue;
		}

		std::size_t read = start;
		std::size_t written = 0;
		bool valid = true;
		while (read < end) {
			const std::uint8_t code = wire[read++];
			const std::size_t count = static_cast<std::size_t>(code - 1u);
			if (count > end - read) {
				valid = false;
				break;
			}
			for (std::size_t i = 0; i < count; ++i) {
				decoded[written++] = wire[read++];
			}
			if (code != 0xFFu && read < end) {
				decoded[written++] = 0u;
			}
		}
		if (valid) {
			transcript.frame({decoded.data(), written});
		} else {
			transcript.malformed();
		}
		start = end + 1u;
	}
	return transcript;
}

Transcript production_decode(const std::span<const std::uint8_t> wire,
	const std::size_t input_chunk,
	const std::size_t output_chunk,
	const bool prearmed)
{
	Transcript transcript;
	cobs::codec::Decoder decoder;
	std::array<std::uint8_t, 32> output{};
	std::size_t next_output = 0;
	const auto prepare_first = [&] {
		const std::size_t granted = std::min(output_chunk, output.size());
		decoder.prepare_output({output.data(), granted});
		next_output = granted;
	};
	if (prearmed) {
		prepare_first();
	}
	std::size_t pos = 0;
	while (pos < wire.size()) {
		const std::size_t end = std::min(wire.size(), pos + input_chunk);
		while (pos < end) {
			const auto result = decoder.consume(wire.subspan(pos, end - pos));
			pos += result.consumed;
			switch (result.event) {
			case cobs::codec::Decoder::Event::NeedOutput: {
				if (next_output == output.size()) {
					std::abort();
				}
				const std::size_t granted =
					std::min(output_chunk, output.size() - next_output);
				decoder.attach_output({output.data() + next_output, granted});
				next_output += granted;
				break;
			}
			case cobs::codec::Decoder::Event::FrameComplete:
				transcript.frame({output.data(), result.decoded_size});
				next_output = 0;
				if (prearmed) {
					prepare_first();
				}
				break;
			case cobs::codec::Decoder::Event::Malformed:
				transcript.malformed();
				next_output = 0;
				if (prearmed) {
					prepare_first();
				}
				break;
			case cobs::codec::Decoder::Event::None:
				break;
			}
			if (result.consumed == 0u &&
				result.event == cobs::codec::Decoder::Event::None) {
				std::abort(); // a non-empty input slice must always make progress
			}
		}
	}
	return transcript;
}

void print_bytes(const std::span<const std::uint8_t> bytes)
{
	for (const std::uint8_t byte : bytes) {
		std::printf("%02X ", static_cast<unsigned>(byte));
	}
	std::printf("\n");
}

std::size_t exhaustive_decoder()
{
	struct Plan {
		std::size_t input;
		std::size_t output;
		bool prearmed;
	};
	constexpr std::array<std::uint8_t, 7> alphabet{
		0u, 1u, 2u, 3u, 0x55u, 0xFEu, 0xFFu};
	constexpr std::array<Plan, 10> plans{{
		{8u, 32u, false}, // whole short stream, one output segment
		{1u, 32u, false}, // one input byte at a time
		{8u, 1u, false},  // every decoded byte crosses an output seam
		{1u, 1u, false},  // both seams at their most fragmented
		{2u, 2u, false},
		{3u, 3u, false},
		{8u, 32u, true},  // same state space with the first segment prepared
		{1u, 32u, true},
		{8u, 1u, true},
		{1u, 1u, true},
	}};

	std::array<std::uint8_t, 7> wire{};
	std::size_t cases = 0;
	std::size_t combinations = 1;
	for (std::size_t length = 0; length <= wire.size(); ++length) {
		if (length != 0u) {
			combinations *= alphabet.size();
		}
		for (std::size_t ordinal = 0; ordinal < combinations; ++ordinal) {
			std::size_t digits = ordinal;
			for (std::size_t i = 0; i < length; ++i) {
				wire[i] = alphabet[digits % alphabet.size()];
				digits /= alphabet.size();
			}
			const std::span<const std::uint8_t> input{wire.data(), length};
			const Transcript expected = reference_decode(input);
			for (const auto& plan : plans) {
				const Transcript actual = production_decode(
					input, plan.input, plan.output, plan.prearmed);
				if (!(actual == expected)) {
					std::printf(
						"decoder mismatch length=%zu ordinal=%zu input=%zu output=%zu prearmed=%d\n",
						length, ordinal, plan.input, plan.output,
						plan.prearmed ? 1 : 0);
					print_bytes(input);
					std::abort();
				}
			}
			++cases;
		}
	}
	return cases;
}

std::size_t exhaustive_encoder()
{
	constexpr std::array<std::uint8_t, 3> alphabet{0u, 1u, 0xFFu};
	std::array<std::uint8_t, 10> raw{};
	std::size_t cases = 0;
	std::size_t combinations = 1;
	for (std::size_t length = 0; length <= raw.size(); ++length) {
		if (length != 0u) {
			combinations *= alphabet.size();
		}
		for (std::size_t ordinal = 0; ordinal < combinations; ++ordinal) {
			std::size_t digits = ordinal;
			for (std::size_t i = 0; i < length; ++i) {
				raw[i] = alphabet[digits % alphabet.size()];
				digits /= alphabet.size();
			}
			const std::vector<std::uint8_t> payload(raw.begin(), raw.begin() + length);
			const std::vector<std::uint8_t> expected = cobs_test::encode(payload);
			for (const std::size_t slack : {std::size_t{0}, std::size_t{1}}) {
				const std::size_t offset = cobs::codec::raw_offset(length) + slack;
				std::vector<std::uint8_t> storage(offset + length);
				std::copy(payload.begin(), payload.end(), storage.begin() + offset);
				const auto frame = cobs::codec::encode_in_place(storage, offset, length);
				if (!std::equal(frame.begin(), frame.end(),
				                expected.begin(), expected.end())) {
					std::printf(
						"encoder mismatch length=%zu ordinal=%zu slack=%zu\n",
						length, ordinal, slack);
					print_bytes(payload);
					std::abort();
				}
				++cases;
			}
		}
	}
	return cases;
}

} // namespace

int main()
{
	const std::size_t decoder_cases = exhaustive_decoder();
	const std::size_t encoder_cases = exhaustive_encoder();
	std::printf("decoder: %zu exhaustive streams under 10 segmentation/prearm plans\n",
		decoder_cases);
	std::printf("encoder: %zu exhaustive payload/headroom cases\n", encoder_cases);
	return 0;
}
