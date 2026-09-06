/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * A storage specification whose bound storage lacks the TX operations must
 * fail where Endpoint applies the shared wire::Storage contract.
 */
#include "modbus/rtu/Rtu.h"

#include <cstddef>

struct MissingTx {
	template<class Geometry>
	class For final {
	public:
		std::byte* acquire_rx(std::size_t) noexcept;
		void release_rx(std::byte*) noexcept;
	};
};

modbus::rtu::Endpoint<MissingTx> endpoint;
