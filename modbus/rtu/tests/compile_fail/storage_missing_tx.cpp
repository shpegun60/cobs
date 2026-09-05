/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"

struct MissingTx {
	using RxBlock = modbus::rtu::RxBlock<MissingTx>;
	static constexpr std::size_t max_adu_size = 256u;
	RxBlock* acquire_rx(std::size_t) noexcept;
	void release_rx(RxBlock*) noexcept;
};

modbus::rtu::Endpoint<MissingTx> endpoint;
