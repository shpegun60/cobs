/*
 * A storage extension without both TX operations must fail where Endpoint
 * applies the public cobs::Storage contract.
 */
#include "Cobs.h"

#include <cstddef>

struct RxOnlyStorage final {
	using Format = cobs::Format<8>;
	using RxBlock = cobs::RxBlock<RxOnlyStorage>;

	[[nodiscard]] RxBlock* acquire_rx(std::size_t) noexcept { return nullptr; }
	void release_rx(RxBlock*) noexcept {}
};

cobs::Endpoint<RxOnlyStorage> invalid_endpoint;
