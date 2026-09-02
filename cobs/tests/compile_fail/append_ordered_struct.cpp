/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Explicit byte order must not make padded ABI objects serializable. */

#include "Cobs.h"

#include <cstdint>

struct WireRecord final {
	uint8_t type;
	uint32_t value;
};

void serialize_struct(cobs::Endpoint<>& endpoint)
{
	auto message = endpoint.make_message();
	const WireRecord record{1u, 2u};
	(void)message.append_be(record);
}
