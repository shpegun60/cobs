/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "adapters/freertos/FreeRtosWake.h"
#include <array>
#include <cstdio>
#include <limits>

int main()
{
	unsigned checks = 0u, failures = 0u;
	const auto check = [&](uint32_t ms) {
		const uint64_t exact = static_cast<uint64_t>(ms) * configTICK_RATE_HZ / 1000u;
		constexpr uint64_t limit = std::numeric_limits<TickType_t>::max() - uint64_t{1u};
		const uint64_t expected = exact < limit ? exact : limit;
		(void)uart::FreeRtosWake::wait(ms);
		++checks;
		if (fake_freertos::model().last_take_timeout != expected) {
			++failures; std::printf("FAIL duration %u\n", static_cast<unsigned>(ms));
		}
	};
	for (const uint32_t ms : std::array<uint32_t, 13u>{0u, 1u, 5u, 50u, 1000u, 65534u, 65535u, 65536u,
	                         4294967u, 4294968u, 7200000u, UINT32_MAX - 1u, UINT32_MAX}) {
		check(ms);
	}
	// Broad deterministic domain, including values far above the old product
	// overflow. No sleeps: the recording kernel captures the real wait argument.
	uint32_t state = 0x13579BDFu;
	for (unsigned i = 0u; i < 100000u; ++i) {
		state = state * 1664525u + 1013904223u; check(state);
	}
	std::printf("Wake ticks: %zu-bit / %u Hz, %u checks, %u failures\n",
	            sizeof(TickType_t) * 8u, static_cast<unsigned>(configTICK_RATE_HZ), checks, failures);
	return failures == 0u ? 0 : 1;
}
