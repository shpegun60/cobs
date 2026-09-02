/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#ifndef MODBUS_RTU_STATS_H_
#define MODBUS_RTU_STATS_H_

#include <cstdint>

namespace modbus::rtu {

struct Stats final {
	struct Rx final {
		uint32_t candidates = 0;
		uint32_t frames_received = 0;
		uint32_t crc_errors = 0;
		uint32_t too_short = 0;
		uint32_t oversize = 0;
		uint32_t allocation_failure = 0;
		uint32_t stream_gaps = 0;
	};

	struct Tx final {
		uint32_t frames_sent = 0;
		uint32_t send_refused_busy = 0;
		uint32_t send_failed = 0;
	};

	Rx rx{};
	Tx tx{};
};

} // namespace modbus::rtu

#endif /* MODBUS_RTU_STATS_H_ */
