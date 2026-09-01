/*
 * Repeatable host microbenchmarks for the two byte-hot COBS codec loops.
 *
 * This is not a pass/fail timing test. It reports best-of-N throughput so a
 * proposed hot-path change can be compared on the same machine, compiler and
 * flags. Correctness remains the job of the normal suites and sanitizers.
 */

#include "Codec.h"
#include "reference_encoder.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0;

enum class Pattern { NonZero, Random, SparseZero, AlternatingZero };

std::string_view name(const Pattern pattern) noexcept
{
	switch (pattern) {
	case Pattern::NonZero:        return "nonzero";
	case Pattern::Random:         return "random";
	case Pattern::SparseZero:     return "sparse-zero";
	case Pattern::AlternatingZero:return "alt-zero";
	}
	return "unknown";
}

std::vector<std::uint8_t> make_payload(const std::size_t size, const Pattern pattern)
{
	std::vector<std::uint8_t> payload(size);
	std::uint32_t state = 0x9E3779B9u ^ static_cast<std::uint32_t>(size);
	for (std::size_t i = 0; i < size; ++i) {
		switch (pattern) {
		case Pattern::NonZero:
			payload[i] = static_cast<std::uint8_t>(1u + (i % 255u));
			break;
		case Pattern::Random:
			state ^= state << 13;
			state ^= state >> 17;
			state ^= state << 5;
			payload[i] = static_cast<std::uint8_t>(state);
			break;
		case Pattern::SparseZero:
			payload[i] = static_cast<std::uint8_t>(
				(i % 31u == 7u) ? 0u : 1u + i % 254u);
			break;
		case Pattern::AlternatingZero:
			payload[i] = static_cast<std::uint8_t>((i & 1u) ? 0x5Au : 0u);
			break;
		}
	}
	return payload;
}

template<class Work>
double best_seconds(Work&& work, const int samples = 5)
{
	double best = 1.0e100;
	for (int sample = 0; sample < samples; ++sample) {
		const auto start = Clock::now();
		const std::uint64_t checksum = work();
		const auto stop = Clock::now();
		g_sink = g_sink + checksum;
		const double seconds = std::chrono::duration<double>(stop - start).count();
		best = std::min(best, seconds);
	}
	return best;
}

// Keeping the fallback makes this one source usable for before/after runs
// against revisions predating Decoder::prepare_output(). On those revisions
// attach_output() while Synced is a no-op and the benchmark takes the original
// NeedOutput path.
template<class Decoder>
void prepare_first(Decoder& decoder, const std::span<std::uint8_t> output) noexcept
{
	if constexpr (requires { decoder.prepare_output(output); }) {
		decoder.prepare_output(output);
	} else {
		decoder.attach_output(output);
	}
}

void print_rate(const char* const operation,
	const Pattern pattern,
	const std::size_t size,
	const std::size_t bytes,
	const std::size_t frames,
	const double seconds)
{
	const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
	const double mib_per_second = mib / seconds;
	const double ns_per_frame = seconds * 1.0e9 / static_cast<double>(frames);
	std::printf("%-7s %-8.*s %5zu B  %9.1f MiB/s  %8.1f ns/frame\n",
		operation,
		static_cast<int>(name(pattern).size()), name(pattern).data(),
		size, mib_per_second, ns_per_frame);
}

void bench_encode(const std::vector<std::uint8_t>& payload,
	const Pattern pattern,
	const std::size_t target_bytes)
{
	const std::size_t size = payload.size();
	const std::size_t offset = cobs::codec::raw_offset(size);
	const std::size_t block_size = cobs::codec::max_wire_size(size);
	const std::size_t blocks = std::max<std::size_t>(64u, target_bytes / size);
	std::vector<std::uint8_t> arena(blocks * block_size);

	const auto prepare = [&] {
		for (std::size_t block = 0; block < blocks; ++block) {
			std::memcpy(arena.data() + block * block_size + offset,
			            payload.data(), size);
		}
	};

	// Every sample needs pristine raw input; restoration is deliberately
	// outside the measured interval.
	double best = 1.0e100;
	for (int sample = 0; sample < 5; ++sample) {
		prepare();
		const auto start = Clock::now();
		std::uint64_t checksum = 0;
		for (std::size_t block = 0; block < blocks; ++block) {
			std::span<std::uint8_t> storage{
				arena.data() + block * block_size, block_size};
			const auto frame = cobs::codec::encode_in_place(storage, offset, size);
			checksum += frame.size() + frame.front() + frame.back();
		}
		const auto stop = Clock::now();
		g_sink = g_sink + checksum;
		best = std::min(best, std::chrono::duration<double>(stop - start).count());
	}
	print_rate("encode", pattern, size, size * blocks, blocks, best);
}

void bench_decode(const std::vector<std::uint8_t>& payload,
	const Pattern pattern,
	const std::size_t target_bytes)
{
	const std::vector<std::uint8_t> frame = cobs_test::encode(payload);
	const std::size_t repeats = std::max<std::size_t>(64u, target_bytes / payload.size());
	std::vector<std::uint8_t> stream;
	stream.reserve(frame.size() * repeats);
	for (std::size_t i = 0; i < repeats; ++i) {
		stream.insert(stream.end(), frame.begin(), frame.end());
	}
	std::vector<std::uint8_t> output(payload.size());

	const double seconds = best_seconds([&] {
		cobs::codec::Decoder decoder;
		// The first destination is stable and already known, as it is for the
		// Endpoint length header. Prepare it so this measures the steady-state
		// hot path rather than an avoidable owner round trip at every frame.
		prepare_first(decoder, output);
		std::size_t pos = 0;
		std::size_t completed = 0;
		std::size_t zero_progress = 0;
		std::uint64_t checksum = 0;
		while (pos < stream.size()) {
			const auto result = decoder.consume(
				std::span<const std::uint8_t>{stream.data() + pos, stream.size() - pos});
			pos += result.consumed;
			zero_progress = (result.consumed == 0u) ? zero_progress + 1u : 0u;
			if (zero_progress > 1u) {
				std::abort(); // benchmark owner failed to answer NeedOutput correctly
			}
			switch (result.event) {
			case cobs::codec::Decoder::Event::NeedOutput:
				decoder.attach_output(output);
				break;
			case cobs::codec::Decoder::Event::FrameComplete:
				++completed;
				checksum += result.decoded_size;
				checksum += output.front();
				checksum += output.back();
				prepare_first(decoder, output);
				break;
			case cobs::codec::Decoder::Event::None:
				break;
			case cobs::codec::Decoder::Event::Malformed:
				std::abort();
			}
		}
		if (completed != repeats) {
			std::abort();
		}
		return checksum;
	});

	print_rate("decode", pattern, payload.size(), payload.size() * repeats, repeats, seconds);
}

std::size_t target_bytes()
{
	constexpr std::size_t kDefaultMiB = 16;
	if (const char* const text = std::getenv("COBS_BENCH_MIB")) {
		const unsigned long value = std::strtoul(text, nullptr, 10);
		if (value != 0u) {
			return static_cast<std::size_t>(value) * 1024u * 1024u;
		}
	}
	return kDefaultMiB * 1024u * 1024u;
}

} // namespace

int main()
{
	const std::size_t bytes = target_bytes();
	std::printf("best of 5; approximately %zu MiB/sample; sink=%llu\n",
		bytes / (1024u * 1024u), static_cast<unsigned long long>(g_sink));

	for (const std::size_t size : {std::size_t{32}, std::size_t{256}, std::size_t{1024}}) {
		for (const Pattern pattern : {
			Pattern::NonZero, Pattern::Random, Pattern::SparseZero,
			Pattern::AlternatingZero}) {
			const auto payload = make_payload(size, pattern);
			bench_encode(payload, pattern, bytes);
			bench_decode(payload, pattern, bytes);
		}
	}

	std::printf("sink=%llu\n", static_cast<unsigned long long>(g_sink));
	return 0;
}
