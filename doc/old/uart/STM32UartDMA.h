/*
 * STM32UartDMA.h
 *
 *  Created on: Aug 8, 2024
 *      Author: admin
 */

#ifndef STM32_TOOLS_UART_STM32UARTDMA_H_
#define STM32_TOOLS_UART_STM32UARTDMA_H_

#include "stm32_uart.h"

#if defined(HAL_DMA_MODULE_ENABLED) && (defined(HAL_UART_MODULE_ENABLED) || defined(HAL_USART_MODULE_ENABLED))

#include <functional>

class STM32UartDMA
{

public:
	using RxHandle_t = std::function<void(u8* const data, const reg size)>;
	using TxHandle_t = std::function<void(const status_t status)>;
	using ErrorHandle_t = std::function<void(const status_t status)>;

	STM32UartDMA() = default;
	STM32UartDMA(UART_HandleTypeDef* const huart, const reg rxBufferSize);
	~STM32UartDMA() = default;

	status_t init(UART_HandleTypeDef* const huart, const reg rxBufferSize);
	void setBaud(const u32 baudRate) { UART_SetBaudRate(&m_uart, baudRate); }
	inline bool IsTxBusy() { return UART_IsTxBusy(&m_uart); }
	inline status_t send(u8 const *txBuff, const reg txSize) { return UART_SendBuffer(&m_uart, txBuff, txSize); }
	inline bool isInitialized() const { return m_uart.huart != nullptr; }


	void setRxHandler(const RxHandle_t callback);
	void setTxHandler(const TxHandle_t callback);
	void setErrorHandler(const ErrorHandle_t callback);

private:
	/*
	 * ****************************************************************************************
	 * -------------------------------------- IT ---------------------------------------------
	 * ****************************************************************************************
	 */
	static void uart_rx_callback(u8* const data, const reg size, void* const ctx);
	static void uart_tx_callback(const status_t status, void* const ctx);
	static void uart_error_callback(const status_t status, void* const ctx);

private:
	stm32_DMA_uart_t m_uart = {};

	RxHandle_t m_rx_callback = nullptr;
	TxHandle_t m_tx_callback = nullptr;
	ErrorHandle_t m_error_callback = nullptr;
};

#endif /* if defined DMA and USART or UART  */

#endif /* STM32_TOOLS_UART_STM32UARTDMA_H_ */
