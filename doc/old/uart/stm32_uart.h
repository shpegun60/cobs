/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*************************************************************************//**
 * @file
 * @brief       This file is part of the AFBR-S50 API.
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
 *
 *****************************************************************************/

#ifndef _TOOLS_UART_H
#define _TOOLS_UART_H


/*!***************************************************************************
 * @defgroup    UART UART:Universal Asynchronous Receiver/Transmitter
 * @ingroup     driver
 * @brief       UART Hardware Layer Module
 * @details     Provides a layer for the UART driver functions.
 *              It uses UART interface with DMA
 * @addtogroup  UART
 * @{
 *****************************************************************************/
#include "stm32_uart_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(HAL_DMA_MODULE_ENABLED) && (defined(HAL_UART_MODULE_ENABLED) || defined(HAL_USART_MODULE_ENABLED))

#include "stm32_uart_container.h"

/*!***************************************************************************
 * @brief   Initialize the Universal Asynchronous Receiver/Transmitter
 *          (UART or LPSCI) bus and DMA module
 * @param   -
 * @return  Returns the \link #status_t status\endlink (#STATUS_OK on success).
 *****************************************************************************/
status_t UART_Init(stm32_DMA_uart_t* const self, const stm32_DMA_uart_policy_t policy);
stm32_DMA_uart_t* const UART_new(const stm32_DMA_uart_policy_t policy);

u32 UART_GetBaudRate(stm32_DMA_uart_t* const self);
status_t UART_SetBaudRate(stm32_DMA_uart_t* const self, const u32 baudRate);


/*!***************************************************************************
 * @brief   Reads the transmittion status of the uart2 interface
 * @param   -
 * @return  Boolean value:
 *           - true: device is busy
 *           - false: device is idle
 *****************************************************************************/
static inline bool UART_IsTxBusy(stm32_DMA_uart_t* const self) { return self->isTxBusy_; }


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
status_t UART_SendBuffer_callback(stm32_DMA_uart_t* const self, u8 const* txBuff, const u32 txSize, const uart_tx_callback_t f, void* const ctx);
status_t UART_SendBuffer(stm32_DMA_uart_t* const self, u8 const *txBuff, const u32 txSize);
status_t UART_SetTxCallback(stm32_DMA_uart_t* const self, const uart_tx_callback_t f, void* const ctx);
void UART_resetTx(stm32_DMA_uart_t* const self);

/*!***************************************************************************
 * @brief   Installs an callback function for the byte received event.
 * @param   f The callback function pointer.
 *****************************************************************************/
void UART_SetRxCallback(stm32_DMA_uart_t* const self, const uart_rx_callback_t f, void* const ctx);

/*!***************************************************************************
 * @brief   Installs an callback function for the error occurred event.
 * @param   f The callback function pointer.
 *****************************************************************************/
void UART_SetErrorCallback(stm32_DMA_uart_t* const self, const uart_error_callback_t f, void* const ctx);


/*!***************************************************************************
* IT Callbacks
 *****************************************************************************/
//general in stm32f1xx_it.c
void USER_UART_IRQHandler(UART_HandleTypeDef* const huart);

// rx callback in void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
void UART_RXCallback(stm32_DMA_uart_t* const self);

//tx calback in void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
void UART_TXCallback(stm32_DMA_uart_t* const self);

// error callback in void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
void UART_ErrorCallback(stm32_DMA_uart_t* const self);

#endif /* if defined DMA and USART or UART  */
#ifdef __cplusplus
} // extern "C"
#endif
/*! @} */
#endif /* _TOOLS_UART_H */
