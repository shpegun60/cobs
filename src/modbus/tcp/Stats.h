/* Author: shpegun60; SPDX-License-Identifier: MIT */
#ifndef MODBUS_TCP_STATS_H_
#define MODBUS_TCP_STATS_H_
#include <cstdint>

namespace modbus::tcp {
struct Stats final {
	struct Rx final {
		uint32_t candidates = 0u; // complete six-byte prefixes, including invalid ones
		uint32_t frames_received = 0u;
		uint32_t invalid_protocol = 0u;
		uint32_t invalid_length = 0u;
		uint32_t oversize = 0u;
		uint32_t crc_errors = 0u;
		uint32_t allocation_failure = 0u;
		uint32_t skipped_frames = 0u; // complete frames skipped after allocation failure
		uint32_t stream_gaps = 0u;
	};
	struct Tx final {
		uint32_t frames_sent = 0u;
		uint32_t send_refused_busy = 0u;
		uint32_t send_failed = 0u;
	};
	Rx rx{};
	Tx tx{};
};
} // namespace modbus::tcp
#endif
