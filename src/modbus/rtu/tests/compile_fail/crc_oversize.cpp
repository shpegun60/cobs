/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"

#include <cstddef>
#include <cstdint>
#include <span>

struct OversizeCrc final {
	using value_type = uint8_t;
	static constexpr std::size_t wire_size = 255u;

	uint8_t calculate(std::span<const uint8_t>) noexcept { return 0u; }
	void store(uint8_t*, uint8_t) noexcept {}
	uint8_t load(const uint8_t*) noexcept { return 0u; }
};

modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<OversizeCrc>> endpoint;
