/*
 * uart_regs.h — USART register-map portability layer for all STM32 series.
 *
 * Detection is done by CMSIS bit-definition macros, NOT by family name, so it
 * follows the actual IP block present on the die:
 *
 *   new USART IP  (ISR / RDR / TDR / ICR):  F0, F3, F7, G0, G4, H5, H7,
 *                                           L0, L4, L5, U0, U5, WB, WL, C0, ...
 *   legacy USART IP (SR / DR):              F1, F2, F4, L1
 *
 * Error-flag clearing follows the reference manuals exactly:
 *   - legacy IP: PE/FE/NE/ORE/IDLE are cleared ONLY by the software sequence
 *     "read SR, then read DR" (RM0008 §27.6.1, RM0090 §30.6.1). Writing SR
 *     does not clear them.
 *   - new IP: dedicated write-1-to-clear bits in ICR (PECF/FECF/NECF/ORECF/
 *     IDLECF and RTOCF where the receiver-timeout feature exists).
 */

#ifndef UART_ENGINE_UART_REGS_H_
#define UART_ENGINE_UART_REGS_H_

#include "main.h" // pulls the family HAL and CMSIS device header

#if defined(HAL_UART_MODULE_ENABLED)

/* ------------------------- register map detection ------------------------ */

#if defined(USART_RDR_RDR_Msk) && defined(USART_ISR_PE_Msk)
	/* New USART IP: status in ISR, data in RDR/TDR, clear via ICR. */
#	define UART_ENGINE_NEW_USART_IP 1
#	define UART_SR_REG(h)	((h)->Instance->ISR)
#	define UART_DR_REG(h)	((h)->Instance->RDR)
#elif defined(USART_DR_DR_Msk) && defined(USART_SR_PE_Msk)
	/* Legacy USART IP: status in SR, data in DR. */
#	define UART_ENGINE_NEW_USART_IP 0
#	define UART_SR_REG(h)	((h)->Instance->SR)
#	define UART_DR_REG(h)	((h)->Instance->DR)
#else
#	error "[UART ENGINE]: could not detect the USART IP block (neither ISR/RDR nor SR/DR bit definitions found)"
#endif

/* --------------------- HAL error-code fallback defines ------------------- */
/* Some features (e.g. receiver timeout) do not exist on every family; the
 * missing codes collapse to NONE (0) so they vanish from the OR-masks below. */

#if !defined(HAL_UART_ERROR_NONE)
#	define HAL_UART_ERROR_NONE	0U
#endif
#if !defined(HAL_UART_ERROR_PE)
#	define HAL_UART_ERROR_PE	HAL_UART_ERROR_NONE
#endif
#if !defined(HAL_UART_ERROR_NE)
#	define HAL_UART_ERROR_NE	HAL_UART_ERROR_NONE
#endif
#if !defined(HAL_UART_ERROR_FE)
#	define HAL_UART_ERROR_FE	HAL_UART_ERROR_NONE
#endif
#if !defined(HAL_UART_ERROR_ORE)
#	define HAL_UART_ERROR_ORE	HAL_UART_ERROR_NONE
#endif
#if !defined(HAL_UART_ERROR_RTO)
#	define HAL_UART_ERROR_RTO	HAL_UART_ERROR_NONE
#endif
#if !defined(HAL_UART_ERROR_DMA)
#	define HAL_UART_ERROR_DMA	HAL_UART_ERROR_NONE
#endif

/* RX-line error class: recoverable by restarting reception. */
#define UART_ENGINE_RX_PART \
	(HAL_UART_ERROR_PE | HAL_UART_ERROR_NE | HAL_UART_ERROR_FE | \
	 HAL_UART_ERROR_ORE | HAL_UART_ERROR_RTO)

/* --------------------- RX error/IDLE flag clearing ----------------------- */

#if UART_ENGINE_NEW_USART_IP

	/* Write-1-to-clear via ICR, exactly the bits the RM defines for it.
	 * RTOCF exists only where the receiver-timeout feature is present. */
#	if defined(UART_CLEAR_RTOF)
#		define UART_ENGINE_CLEAR_RX_ERRORS(h) \
			__HAL_UART_CLEAR_FLAG((h), \
				UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF | \
				UART_CLEAR_OREF | UART_CLEAR_IDLEF | UART_CLEAR_RTOF)
#	else
#		define UART_ENGINE_CLEAR_RX_ERRORS(h) \
			__HAL_UART_CLEAR_FLAG((h), \
				UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF | \
				UART_CLEAR_OREF | UART_CLEAR_IDLEF)
#	endif

#else /* legacy IP */

	/* RM0008 §27.6.1 / RM0090 §30.6.1: PE, FE, NE, ORE and IDLE are cleared
	 * by a software sequence: a read from SR followed by a read from DR. */
#	define UART_ENGINE_CLEAR_RX_ERRORS(h) \
		do { \
			volatile uint32_t _sr = UART_SR_REG(h); \
			volatile uint32_t _dr = UART_DR_REG(h); \
			(void)_sr; (void)_dr; \
		} while (0)

#endif /* UART_ENGINE_NEW_USART_IP */

#endif /* HAL_UART_MODULE_ENABLED */
#endif /* UART_ENGINE_UART_REGS_H_ */
