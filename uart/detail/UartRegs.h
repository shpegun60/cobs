/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * UartRegs.h - typed USART register-map helpers for UART internals.
 *
 * Detection follows CMSIS bit definitions rather than STM32 family names:
 *
 *   new USART IP  (ISR / RDR / TDR / ICR): F0, F3, F7, G0, G4, H5, H7,
 *                                           L0, L4, L5, U0, U5, WB, WL, C0
 *   legacy USART IP (SR / DR):              F1, F2, F4, L1
 *
 * Error clearing follows the reference manuals exactly. Legacy IP requires
 * the ordered "read SR, then read DR" sequence; new IP uses ICR write-1-to-
 * clear bits.
 */

#ifndef UART_DETAIL_UART_REGS_H_
#define UART_DETAIL_UART_REGS_H_

#include "main.h"

#include <cstdint>

#if defined(HAL_UART_MODULE_ENABLED)

namespace uart::detail {

#if defined(HAL_UART_ERROR_NONE)
inline constexpr std::uint32_t no_error = HAL_UART_ERROR_NONE;
#else
inline constexpr std::uint32_t no_error = 0u;
#endif

inline constexpr std::uint32_t rx_error_mask =
#if defined(HAL_UART_ERROR_PE)
	HAL_UART_ERROR_PE |
#endif
#if defined(HAL_UART_ERROR_NE)
	HAL_UART_ERROR_NE |
#endif
#if defined(HAL_UART_ERROR_FE)
	HAL_UART_ERROR_FE |
#endif
#if defined(HAL_UART_ERROR_ORE)
	HAL_UART_ERROR_ORE |
#endif
#if defined(HAL_UART_ERROR_RTO)
	HAL_UART_ERROR_RTO |
#endif
	0u;

#if defined(USART_RDR_RDR_Msk) && defined(USART_ISR_PE_Msk)

inline constexpr bool new_usart_ip = true;

inline void clear_rx_errors(UART_HandleTypeDef* const huart) noexcept
{
	// Write-1-to-clear exactly the bits defined by this USART IP.
#if defined(UART_CLEAR_RTOF)
	__HAL_UART_CLEAR_FLAG(huart,
		UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF |
		UART_CLEAR_OREF | UART_CLEAR_IDLEF | UART_CLEAR_RTOF);
#else
	__HAL_UART_CLEAR_FLAG(huart,
		UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF |
		UART_CLEAR_OREF | UART_CLEAR_IDLEF);
#endif
}

#elif defined(USART_DR_DR_Msk) && defined(USART_SR_PE_Msk)

inline constexpr bool new_usart_ip = false;

inline void clear_rx_errors(UART_HandleTypeDef* const huart) noexcept
{
	// RM0008 27.6.1 / RM0090 30.6.1: this read order is the operation.
	volatile std::uint32_t status = huart->Instance->SR;
	volatile std::uint32_t data = huart->Instance->DR;
	(void)status;
	(void)data;
}

#else
#error "[UART ENGINE]: could not detect the USART IP block (neither ISR/RDR nor SR/DR bit definitions found)"
#endif

} // namespace uart::detail

#endif /* HAL_UART_MODULE_ENABLED */
#endif /* UART_DETAIL_UART_REGS_H_ */
