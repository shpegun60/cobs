/*
 * stm32_uart_container.c
 *
 *  Created on: Jun 30, 2024
 *      Author: admin
 */

#include "stm32_uart_container.h"
#include <string.h>
#include "irq/irq_block.h"

#if defined(HAL_DMA_MODULE_ENABLED) && (defined(HAL_UART_MODULE_ENABLED) || defined(HAL_USART_MODULE_ENABLED))

static stm32_DMA_uart_t* m_instances[UART_CONTAINER_COUNT] = {NULL};

#if (UART_CONTAINER_COUNT > 1)
static u8 m_instance_counter = 0;
#endif /* (UART_CONTAINER_COUNT > 1) */


stm32_DMA_uart_t* const getContainerUartInstance(UART_HandleTypeDef* const huart)
{
#if (UART_CONTAINER_COUNT == 1)

	stm32_DMA_uart_t* const r_inst = m_instances[0];
	if(r_inst->huart == huart) {
		return r_inst;
	}

#else
	const u8 r_inst_cnt = m_instance_counter;

	if(huart == NULL) {
		return NULL;
	}

	for(u8 i = 0; i < r_inst_cnt; ++i) {
		stm32_DMA_uart_t* const r_inst = m_instances[i];

		//if(r_inst->huart->Instance == huart->Instance) { // must be optimized
		if(r_inst->huart == huart) {
			return r_inst;
		}
	}
#endif /* (UART_CONTAINER_COUNT == 1) */

	return NULL;
}

status_t pushContainerUartInstance(stm32_DMA_uart_t* const inst)
{
#if (UART_CONTAINER_COUNT == 1)
	if(inst == NULL || m_instances[0] != NULL) {
		return ERROR_FAIL;
	}

	m_instances[0] = inst;
#else
	if(inst == NULL || (m_instance_counter == UART_CONTAINER_COUNT)) {
		return ERROR_FAIL;
	}

	// find the same element ----------------------------------
	{
		const u8 r_inst_cnt = m_instance_counter;

		for(u8 i = 0; i < r_inst_cnt; ++i) {
			stm32_DMA_uart_t* const r_inst = m_instances[i];

			if(r_inst == inst) {
				// if the same is exists then nothing doing
				return STATUS_OK;
			}
		}
	}

	// push to container ---------------------------------------
	{
		IRQ_LOCK();
		m_instances[m_instance_counter] = inst;
		++m_instance_counter;
		IRQ_UNLOCK();
	}

#endif /* (UART_CONTAINER_COUNT == 1) */

	return STATUS_OK;
}


status_t deleteContainerUartInstance(stm32_DMA_uart_t* const inst)
{

#if (UART_CONTAINER_COUNT == 1)
	stm32_DMA_uart_t* const r_inst = m_instances[0];
	if(r_inst == inst) {
		m_instances[0] = NULL;
	}

#else
	// find the element and clear
	{
		const u8 r_inst_cnt = m_instance_counter;
		u8 is_exists = 0;

		for(u8 i = 0; i < r_inst_cnt; ++i) {
			stm32_DMA_uart_t* const r_inst = m_instances[i];

			if(r_inst == inst) {
				m_instances[i] = NULL;
				is_exists = 1;
			}
		}

		if(!is_exists) {
			return ERROR_FAIL;
		}
	}

	// change main buffer
	{
		stm32_DMA_uart_t* m_tmp[UART_CONTAINER_COUNT] = {NULL};
		u8 m_counter = 0;

		for(u8 i = 0; i < UART_CONTAINER_COUNT; ++i) {
			stm32_DMA_uart_t* const r_inst = m_instances[i];

			if(r_inst != NULL) {
				m_tmp[m_counter] = r_inst;
				++m_counter;
			}
		}

		IRQ_LOCK();
		memcpy(m_instances, m_tmp, sizeof(m_instances));
		m_instance_counter = m_counter;
		IRQ_UNLOCK();
	}
#endif

	return STATUS_OK;
}


#endif /* defined(HAL_DMA_MODULE_ENABLED) && (defined(HAL_UART_MODULE_ENABLED) || defined(HAL_USART_MODULE_ENABLED)) */
