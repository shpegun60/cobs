/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * IrqGuard.h - scoped PRIMASK ownership for UART internals.
 */

#ifndef UART_DETAIL_IRQ_GUARD_H_
#define UART_DETAIL_IRQ_GUARD_H_

#include "cmsis_compiler.h"

#include <cstdint>

namespace uart::detail {

class IrqGuard final {
public:
	IrqGuard() noexcept
		: primask_(__get_PRIMASK())
	{
		__disable_irq();
		__DMB();
	}

	~IrqGuard() noexcept
	{
		__DMB();
		__set_PRIMASK(primask_);
	}

	IrqGuard(const IrqGuard&) = delete;
	IrqGuard& operator=(const IrqGuard&) = delete;
	IrqGuard(IrqGuard&&) = delete;
	IrqGuard& operator=(IrqGuard&&) = delete;

private:
	std::uint32_t primask_;
};

} // namespace uart::detail

#endif /* UART_DETAIL_IRQ_GUARD_H_ */
