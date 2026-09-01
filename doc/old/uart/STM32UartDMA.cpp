/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * STM32UartDMA.cpp
 *
 *  Created on: Aug 8, 2024
 *      Author: admin
 */

#include "STM32UartDMA.h"

#if defined(HAL_DMA_MODULE_ENABLED) && (defined(HAL_UART_MODULE_ENABLED) || defined(HAL_USART_MODULE_ENABLED))


STM32UartDMA::STM32UartDMA(UART_HandleTypeDef* const huart, const reg rxBufferSize)
{
	// TODO Auto-generated constructor stub
	init(huart, rxBufferSize);
}

status_t STM32UartDMA::init(UART_HandleTypeDef *const huart, const reg rxBufferSize)
{
	const stm32_DMA_uart_policy_t uart_policy = {
			.huart 		= huart,
			.rxBuff1 	= NULL,
			.rxBuff2 	= NULL,
			.rxBuffSize = rxBufferSize
	};
	const status_t status = UART_Init(&m_uart, uart_policy);

	UART_SetRxCallback(&m_uart, NULL, NULL);
	UART_SetTxCallback(&m_uart, NULL, NULL);
	UART_SetErrorCallback(&m_uart, NULL, NULL);

	return status;
}


void STM32UartDMA::setRxHandler(const RxHandle_t callback)
{
	if(callback) {
		m_rx_callback = callback;
		UART_SetRxCallback(&m_uart, reinterpret_cast<uart_rx_callback_t>(uart_rx_callback), this);
	}
}

void STM32UartDMA::setTxHandler(const TxHandle_t callback)
{
	if(callback) {
		m_tx_callback 				= callback;
		const uint32_t last_time 	= HAL_GetTick();

		while(HAL_GetTick() - last_time < 1000) {
			if(UART_SetTxCallback(&m_uart, uart_tx_callback, this) == STATUS_OK) {
				return;
			}
		}
	}
}

void STM32UartDMA::setErrorHandler(const ErrorHandle_t callback)
{
	if(callback) {
		m_error_callback = callback;
		UART_SetErrorCallback(&m_uart, uart_error_callback, this);
	}
}

/*
 * ****************************************************************************************
 * -------------------------------------- IT ---------------------------------------------
 * ****************************************************************************************
 */
void STM32UartDMA::uart_rx_callback(u8* const data, reg const size, void* const ctx)
{
	STM32UartDMA* const self = static_cast<STM32UartDMA*>(ctx);
	self->m_rx_callback(data, size);
}

void STM32UartDMA::uart_tx_callback(const status_t status, void* const ctx)
{
	STM32UartDMA* const self = static_cast<STM32UartDMA*>(ctx);
	self->m_tx_callback(status);
}

void STM32UartDMA::uart_error_callback(const status_t status, void* const ctx)
{
	STM32UartDMA* const self = static_cast<STM32UartDMA*>(ctx);
	self->m_error_callback(status);
}


#endif /* if defined DMA and USART or UART  */
