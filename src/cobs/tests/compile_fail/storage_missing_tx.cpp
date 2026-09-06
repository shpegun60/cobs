/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * A storage specification whose bound storage lacks the TX operations must
 * fail where Endpoint applies the shared wire::Storage contract, not deep
 * inside the message or receiver templates.
 */
#include "Cobs.h"

#include <cstddef>

struct RxOnlyStorage final {
	template<class Geometry>
	class For final {
	public:
		[[nodiscard]] std::byte* acquire_rx(std::size_t) noexcept { return nullptr; }
		void release_rx(std::byte*) noexcept {}
	};
};

cobs::Endpoint<RxOnlyStorage> invalid_endpoint;
