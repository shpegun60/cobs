/* Author: shpegun60; SPDX-License-Identifier: MIT */
#include "Cobs.h"
struct Huge {
	using value_type = uint8_t;
	static constexpr std::size_t wire_size = 300;
	uint8_t calculate(std::span<const uint8_t>) noexcept;
	void store(uint8_t*, uint8_t) noexcept;
	uint8_t load(const uint8_t*) noexcept;
};
cobs::Endpoint<wire::Heap, cobs::Format<Huge>> endpoint;
