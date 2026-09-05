/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * Encoding is a coordinator transition, not an application Message method.
 */
#include "Cobs.h"

void violate_message_boundary(cobs::Endpoint<>& endpoint)
{
	auto message = endpoint.make_message();
	crc::Crc16Bitwise crc;
	(void)message.encode(crc);
}
