/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

#include "modbus/rtu/Rtu.h"

struct WrongGeometry final {
	using RxBlock = modbus::rtu::RxBlock<WrongGeometry>;
	static constexpr std::size_t max_adu_size = 255u;

	RxBlock* acquire_rx(std::size_t) noexcept;
	void release_rx(RxBlock*) noexcept;
	modbus::rtu::TxBlock acquire_tx(std::size_t) noexcept;
	void release_tx(modbus::rtu::TxBlock) noexcept;
};

modbus::rtu::Endpoint<WrongGeometry> endpoint;

int main() {}
