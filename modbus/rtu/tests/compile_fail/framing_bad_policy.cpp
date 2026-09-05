/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * A framer type that names a direction but no layout() is not a framing
 * policy; the endpoint must say so instead of failing somewhere inside the
 * stream receiver.
 */
#include "modbus/rtu/Rtu.h"

struct HalfPolicy {
	static constexpr modbus::rtu::framing::Direction rx =
		modbus::rtu::framing::Direction::Request;
};

modbus::rtu::Endpoint<wire::Heap, modbus::rtu::Format<>, HalfPolicy> endpoint;
