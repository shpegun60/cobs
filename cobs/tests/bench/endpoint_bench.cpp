/*
 * End-to-end RX/TX hot-path microbenchmark over deterministic fixed storage.
 * Timing is informational; correctness is proved by the normal suites.
 */

#include "Cobs.h"
#include "reference_frame.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

using Format = cobs::Format<1024>;
using Storage = cobs::Pool<8, 1, Format>;
using Endpoint = cobs::Endpoint<Storage>;
using Clock = std::chrono::steady_clock;

volatile std::uint64_t g_sink = 0;

std::vector<std::uint8_t> payload(const std::size_t size)
{
	std::vector<std::uint8_t> bytes(size);
	for (std::size_t i = 0; i < size; ++i) {
		bytes[i] = static_cast<std::uint8_t>(
			(i % 31u == 7u) ? 0u : 1u + i % 254u);
	}
	return bytes;
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

template<class Work>
double best_seconds(Work&& work)
{
	double best = 1.0e100;
	for (int sample = 0; sample < 5; ++sample) {
		const auto start = Clock::now();
		const std::uint64_t checksum = work();
		const auto stop = Clock::now();
		g_sink = g_sink + checksum;
		best = std::min(best, std::chrono::duration<double>(stop - start).count());
	}
	return best;
}

void print_rate(const char* const operation,
	const std::size_t size,
	const std::size_t frames,
	const double seconds)
{
	const double mib = static_cast<double>(size * frames) / (1024.0 * 1024.0);
	std::printf("%-2s %5zu B  %9.1f MiB/s  %8.1f ns/frame\n",
		operation, size, mib / seconds,
		seconds * 1.0e9 / static_cast<double>(frames));
}

void bench_rx(const std::vector<std::uint8_t>& body,
	const std::size_t bytes)
{
	const auto frame = cobs_test::frame(body, Format::length_size);
	const std::size_t frames =
		std::max<std::size_t>(64u, bytes / body.size());
	const double seconds = best_seconds([&] {
		Endpoint endpoint;
		std::uint64_t checksum = 0;
		for (std::size_t i = 0; i < frames; ++i) {
			endpoint.consume(frame);
			{
				auto packet = endpoint.pop_packet();
				if (!packet || packet.size() != body.size()) {
					std::abort();
				}
				checksum += packet.size();
				checksum += packet.data().front();
				checksum += packet.data().back();
			}
		}
		return checksum;
	});
	print_rate("rx", body.size(), frames, seconds);
}

struct Transport {
	std::uint64_t checksum = 0;

	bool start(const std::span<const std::uint8_t> frame) noexcept
	{
		checksum += frame.size() + frame.front() + frame.back();
		return true;
	}

	bool busy() const noexcept { return false; }
};

void bench_tx(const std::vector<std::uint8_t>& body,
	const std::size_t bytes)
{
	const std::size_t frames =
		std::max<std::size_t>(64u, bytes / body.size());
	const double seconds = best_seconds([&] {
		Endpoint endpoint;
		Transport transport;
		if (!endpoint.bind(
			typename Endpoint::Sender{tiny::bind<&Transport::start>(transport)},
			typename Endpoint::BusyQuery{tiny::bind<&Transport::busy>(transport)})) {
			std::abort();
		}
		for (std::size_t i = 0; i < frames; ++i) {
			auto message = endpoint.make_message(body.size());
			if (!message.append_bytes(body) ||
				endpoint.send(message) != cobs::SendResult::Sent) {
				std::abort();
			}
			endpoint.poll();
		}
		return transport.checksum;
	});
	print_rate("tx", body.size(), frames, seconds);
}

} // namespace

int main()
{
	const std::size_t bytes = target_bytes();
	std::printf("best of 5; approximately %zu MiB/sample; sink=%llu\n",
		bytes / (1024u * 1024u), static_cast<unsigned long long>(g_sink));
	for (const std::size_t size : {
		std::size_t{32}, std::size_t{256}, std::size_t{1024}}) {
		const auto body = payload(size);
		bench_rx(body, bytes);
		bench_tx(body, bytes);
	}
	std::printf("sink=%llu\n", static_cast<unsigned long long>(g_sink));
	return 0;
}
