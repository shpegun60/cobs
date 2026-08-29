/*
 * stm32_uart_container.h
 *
 *  Created on: Jun 30, 2024
 *      Author: admin
 */

#ifndef TOOLS_UART_STM32_UART_CONTAINER_H_
#define TOOLS_UART_STM32_UART_CONTAINER_H_


#include "stm32_uart_config.h"

#if defined(HAL_DMA_MODULE_ENABLED) && (defined(HAL_UART_MODULE_ENABLED) || defined(HAL_USART_MODULE_ENABLED))

#define UART_CONTAINER_COUNT 1

stm32_DMA_uart_t* const getContainerUartInstance(UART_HandleTypeDef* const huart);
status_t pushContainerUartInstance(stm32_DMA_uart_t* const inst);
status_t deleteContainerUartInstance(stm32_DMA_uart_t* const inst);


#endif /* defined(HAL_DMA_MODULE_ENABLED) && (defined(HAL_UART_MODULE_ENABLED) || defined(HAL_USART_MODULE_ENABLED)) */

#endif /* TOOLS_UART_STM32_UART_CONTAINER_H_ */
