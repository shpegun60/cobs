/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Native serialization must not silently copy padding and ABI field layout.
 */
#include "Cobs.h"

#include <cstdint>

struct WireRecord final {
	uint8_t type;
	uint32_t value;
};

void serialize_struct(cobs::Endpoint<>& endpoint)
{
	auto message = endpoint.make_message();
	const WireRecord record{1, 2};
	(void)message.append_native(record);
}
