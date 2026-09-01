/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*************************************************************************//**
 * @file
 * @brief       This file is part of the STM32F401RE platform layer.
 * @details     This file provides UART driver functionality.
 *
 * @copyright
 *
 * Copyright (c) 2021, Broadcom Inc
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *****************************************************************************/

#include "stm32_uart.h"

#if defined(HAL_DMA_MODULE_ENABLED) && (defined(HAL_UART_MODULE_ENABLED) || defined(HAL_USART_MODULE_ENABLED))

#include "irq/irq_block.h"

#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U) && defined(SCB_CCR_DC_Msk)
static inline bool uart_dcache_enabled(void)
{
	return (SCB->CCR & SCB_CCR_DC_Msk) != 0u;
}

static inline void uart_dcache_invalidate(const void* const addr, const u32 size)
{
	if ((addr == NULL) || (size == 0u) || !uart_dcache_enabled()) {
		return;
	}

	SCB_InvalidateDCache_by_Addr((void*)addr, (int32_t)size);
}

static inline void uart_dcache_clean(const void* const addr, const u32 size)
{
	if ((addr == NULL) || (size == 0u) || !uart_dcache_enabled()) {
		return;
	}

	SCB_CleanDCache_by_Addr((void*)addr, (int32_t)size);
}
#else
static inline void uart_dcache_invalidate(const void* const addr, const u32 size)
{
	(void)addr;
	(void)size;
}

static inline void uart_dcache_clean(const void* const addr, const u32 size)
{
	(void)addr;
	(void)size;
}
#endif


/*!***************************************************************************
 * @brief   Initialize the Universal Asynchronous Receiver/Transmitter
 *          (UART or LPSCI) bus and DMA module
 * @param   -
 * @return  Returns the \link #status_t status\endlink (#STATUS_OK on success).
 *****************************************************************************/

status_t UART_Init(stm32_DMA_uart_t* const self, const stm32_DMA_uart_policy_t policy)
{
	if((self == NULL) || (policy.huart == NULL) || (policy.rxBuffSize > UART_MAX_BUFF_SIZE) ||
			(policy.rxBuffSize == 0) || !policy.huart->hdmatx || !policy.huart->hdmarx) {

		Error_Handler();
		return ERROR_INVALID_ARGUMENT;
	}

	memset(self, 0, sizeof(stm32_DMA_uart_t));
	self->huart 		= policy.huart;
	self->isTxBusy_ 	= true;

	/* if buffers in policy is null than allocate buffers */
	if(policy.rxBuff1 == NULL || policy.rxBuff2 == NULL) {

		void* const buf1 = malloc(policy.rxBuffSize);
		if(buf1 == NULL) {
			Error_Handler();
			return ERROR_FAIL;
		}

		void* const buf2 = malloc(policy.rxBuffSize);
		if(buf2 == NULL) {
			free(buf1);
			Error_Handler();
			return ERROR_FAIL;
		}

		self->RX.rxBuffer1 		= buf1;
		self->RX.rxBuffer2 		= buf2;
		self->RX.rxBufferSize 	= policy.rxBuffSize;

		if(pushContainerUartInstance(self) != STATUS_OK) {
			free(buf1);
			free(buf2);
			memset(self, 0, sizeof(stm32_DMA_uart_t));
			self->isTxBusy_ = true;
			Error_Handler();
			return ERROR_FAIL;
		}

		//HAL_UART_Receive_DMA(huart, self->RX.rxBuffer1, rxBufferSize);
		policy.huart->pRxBuffPtr 	= buf1;
	} else {
		self->RX.rxBuffer1 		= policy.rxBuff1;
		self->RX.rxBuffer2 		= policy.rxBuff2;
		self->RX.rxBufferSize 	= policy.rxBuffSize;


		if(pushContainerUartInstance(self) != STATUS_OK) {
			memset(self, 0, sizeof(stm32_DMA_uart_t));
			self->isTxBusy_ = true;
			Error_Handler();
			return ERROR_FAIL;
		}

		//HAL_UART_Receive_DMA(huart, self->RX.rxBuffer1, rxBufferSize);
		policy.huart->pRxBuffPtr 	= policy.rxBuff1;
	}


	self->isTxBusy_ 	= false;
	UART_SetRxCallback(self, NULL, NULL);

	return STATUS_OK;
}

stm32_DMA_uart_t* const UART_new(const stm32_DMA_uart_policy_t policy)
{
	stm32_DMA_uart_t* const m = malloc(sizeof(stm32_DMA_uart_t));
	if(m == NULL) {
		return NULL;
	}

	if(UART_Init(m, policy) != STATUS_OK) {
		free(m);
		return NULL;
	}

	return m;
}


u32 UART_GetBaudRate(stm32_DMA_uart_t* const self)
{
	return self->huart->Init.BaudRate;
}

status_t UART_SetBaudRate(stm32_DMA_uart_t* const self, const u32 baudRate)
{
	/* Check module state; TX line must be idle to rest baud rate... */
	IRQ_LOCK();
	if (self->isTxBusy_) {
		IRQ_UNLOCK();
		return STATUS_BUSY;
	}

	self->isTxBusy_ = true;
	IRQ_UNLOCK();

	/* remove callback and disable RX line. */
	const uart_rx_callback_t callback 	= self->RX.rxCallback_;
	void* const ctx 					= self->RX.ctx;
	UART_SetRxCallback(self, NULL, NULL);

	/* Obtain correct baud rate setting value. */
	self->huart->Init.BaudRate = baudRate;
	HAL_UART_Init(self->huart);

	/* Add callback and enable RX line again. */
	UART_SetRxCallback(self, callback, ctx);

	self->isTxBusy_ = false;

	return STATUS_OK;
}


/*!***************************************************************************
 * @brief   Writes several bytes to the UART connection.
 * @param   txBuff Data array to write to the uart connection
 * @param   txSize The size of the data array
 * @param   f Callback function after tx is done, set 0 if not needed;
 * @param   state Optional user state that will be passed to callback
 *                  function; set 0 if not needed.
 * @return  Returns the \link #status_t status\endlink:
 *           - #STATUS_OK (0) on success.
 *           - #STATUS_BUSY on Tx line busy
 *           - #ERROR_NOT_INITIALIZED
 *           - #ERROR_INVALID_ARGUMENT
 *****************************************************************************/
status_t UART_SendBuffer_callback(stm32_DMA_uart_t* const self, u8 const* txBuff, const u32 txSize, const uart_tx_callback_t f, void* const ctx)
{
	/* Verify arguments. */
	if (!txBuff || txSize == 0) {
		return ERROR_INVALID_ARGUMENT;
	}

	/* Lock interrupts to prevent completion interrupt before setup is complete */
	//IRQ_LOCK();

	if (self->isTxBusy_) {
		//IRQ_UNLOCK();
		return STATUS_BUSY;
	}

	/* Set callbacks */
	self->TX.txCallback_ 	= f;
	self->TX.ctx 			= ctx;

	uart_dcache_clean(txBuff, txSize);
	const HAL_StatusTypeDef status = HAL_UART_Transmit_DMA(self->huart, txBuff, txSize);

	if (status != HAL_OK) {
		//IRQ_UNLOCK();
		return ERROR_FAIL;
	}

	/* Set Tx Busy Status. */
	self->isTxBusy_ 		= true;
	//IRQ_UNLOCK(); // this must come after HAL_UART_Transmit_DMA to avoid race conditions w/ IRQs

	return STATUS_OK;
}

status_t UART_SendBuffer(stm32_DMA_uart_t* const self, u8 const *txBuff, const u32 txSize)
{
	/* Verify arguments. */
	if (!txBuff || txSize == 0) {
		return ERROR_INVALID_ARGUMENT;
	}

	/* Lock interrupts to prevent completion interrupt before setup is complete */
	//IRQ_LOCK();

	if (self->isTxBusy_) {
		//IRQ_UNLOCK();
		return STATUS_BUSY;
	}

	uart_dcache_clean(txBuff, txSize);
	const HAL_StatusTypeDef status = HAL_UART_Transmit_DMA(self->huart, (u8*) txBuff, txSize);

	if (status != HAL_OK) {
		//IRQ_UNLOCK();
		return ERROR_FAIL;
	}

	/* Set Tx Busy Status. */
	self->isTxBusy_ = true;
	//IRQ_UNLOCK(); // this must come after HAL_UART_Transmit_DMA to avoid race conditions w/ IRQs

	return STATUS_OK;
}

status_t UART_SetTxCallback(stm32_DMA_uart_t* const self, const uart_tx_callback_t f, void* const ctx)
{
	if (self->isTxBusy_) {
		return STATUS_BUSY;
	}

	self->TX.txCallback_ = f;
	self->TX.ctx = ctx;

	return STATUS_OK;
}

void UART_resetTx(stm32_DMA_uart_t* const self)
{
	UART_HandleTypeDef* const huart = self->huart;

	IRQ_LOCK();
	HAL_UART_AbortTransmit(huart);
	self->isTxBusy_ = false;
	IRQ_UNLOCK();
}

/*!***************************************************************************
 * @brief   Installs an callback function for the byte received event.
 * @param   f The callback function pointer.
 *****************************************************************************/
void UART_SetRxCallback(stm32_DMA_uart_t* const self, const uart_rx_callback_t f, void* const ctx)
{
	UART_HandleTypeDef* const huart = self->huart;

	self->RX.rxCallback_ = f;
	self->RX.ctx = ctx;

	/* Start receiving */
	if (f) {
		u8* const r_rxBuffer = self->RX.rxBuffer1;
		const u32 r_buff_size = self->RX.rxBufferSize;

		uart_dcache_invalidate(r_rxBuffer, r_buff_size);
		HAL_UART_Receive_DMA(huart, r_rxBuffer, r_buff_size); 	// Start receiving via DMA
		__HAL_UART_ENABLE_IT(huart, UART_IT_IDLE);  			// Enable serial port idle interrupt
	} else {
		HAL_UART_AbortReceive(huart);
		UART_RXCallback(self);
		__HAL_UART_DISABLE_IT(huart, UART_IT_IDLE);  			// Disable serial port idle interrupt
	}
}

/*!***************************************************************************
 * @brief   Installs an callback function for the error occurred event.
 * @param   f The callback function pointer.
 *****************************************************************************/
void UART_SetErrorCallback(stm32_DMA_uart_t* const self, const uart_error_callback_t f, void* const ctx)
{
	self->Error.errorCallback = f;
	self->Error.ctx = ctx;
}


/*!***************************************************************************
 * IT Callbacks
 *****************************************************************************/

//general in stm32f1xx_it.c
void USER_UART_IRQHandler(UART_HandleTypeDef* const huart)
{
	if (RESET != __HAL_UART_GET_FLAG(huart, UART_FLAG_IDLE)) {

		stm32_DMA_uart_t* const self = getContainerUartInstance(huart);
		if(self == NULL) {
			return;
		}

		// On idle interruption
		__HAL_UART_CLEAR_IDLEFLAG(huart); // Clear idle interrupt sign
		HAL_UART_AbortReceive(huart);
		UART_RXCallback(self);
	}
}

// rx callback in void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
void UART_RXCallback(stm32_DMA_uart_t* const self)
{
	UART_HandleTypeDef* const huart = self->huart;
	u8* const r_rxBuffer1 = self->RX.rxBuffer1;
	u8* const r_rxBuffer2 = self->RX.rxBuffer2;
	const u32 r_buff_size = self->RX.rxBufferSize;

	const u32 size = r_buff_size - __HAL_DMA_GET_COUNTER(huart->hdmarx);
	u8* const curr = huart->pRxBuffPtr;
	u8* const next = (huart->pRxBuffPtr == r_rxBuffer1) ? r_rxBuffer2 : r_rxBuffer1;

	uart_dcache_invalidate(next, r_buff_size);
	const HAL_StatusTypeDef rtn = HAL_UART_Receive_DMA(huart, next, r_buff_size);
	if (rtn != HAL_OK || huart->gState == HAL_UART_STATE_ERROR) {
		UART_ErrorCallback(self);
		return;
	}

	uart_dcache_invalidate(curr, size);

	void* const _ctx = self->RX.ctx;
	const uart_rx_callback_t _callback = self->RX.rxCallback_;
	if (_callback && size) {
		_callback(curr, size, _ctx);
	}
}

/**
 * @brief  Tx Transfer completed callbacks.
 * @param  huart  Pointer to a UART_HandleTypeDef structure that contains
 *                the configuration information for the specified UART module.
 * @retval None
 */
//tx callback in void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
void UART_TXCallback(stm32_DMA_uart_t* const self)
{
	void* const _ctx = self->TX.ctx;
	const uart_tx_callback_t _callback = self->TX.txCallback_;

	const status_t status = (self->huart->gState == HAL_UART_STATE_ERROR) ? ERROR_FAIL : STATUS_OK;
	self->isTxBusy_ = false;

	if (_callback) {
		_callback(status, _ctx);
	}
}

// error callback in void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
void UART_ErrorCallback(stm32_DMA_uart_t* const self)
{
	UART_HandleTypeDef* const huart = self->huart;
	status_t status = ERROR_FAIL;

	// Errors check
	if (huart->ErrorCode & HAL_UART_ERROR_ORE) {
		status = ERROR_UART_RX_OVERRUN;
	} else if (huart->ErrorCode & HAL_UART_ERROR_FE) {
		status = ERROR_UART_FRAMING_ERR;
	} else if (huart->ErrorCode & HAL_UART_ERROR_NE) {
		status = ERROR_UART_RX_NOISE;
	} else if (huart->ErrorCode & HAL_UART_ERROR_DMA) {
		self->isTxBusy_ = false;
		status = ERROR_UART_TX_DMA_ERR;
	} else if (huart->gState == HAL_UART_STATE_ERROR) {
		self->isTxBusy_ = false;
		status = ERROR_FAIL;
	} else if (huart->gState == HAL_UART_STATE_TIMEOUT) {
		self->isTxBusy_ = false;
		status = ERROR_TIMEOUT;
	} else if (huart->gState == HAL_UART_STATE_BUSY_TX ||
			huart->gState == HAL_UART_STATE_BUSY_TX_RX ||
			huart->gState == HAL_UART_STATE_BUSY_RX ||
			huart->gState == HAL_UART_STATE_BUSY) {
		// If only transmission or transmission and reception, we do not change anything
		status = STATUS_BUSY;
	}

	// Call the custom callback if it is defined
	void* const _ctx = self->Error.ctx;
	const uart_error_callback_t _callback = self->Error.errorCallback;
	if (_callback) {
		_callback(status, _ctx);
	}
}

#endif /* if defined DMA and USART or UART  */
