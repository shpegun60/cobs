// Stand-in for the CubeMX symbols the INTEGRATION.md snippets use, on the
// host fake HAL: a configured UART handle pair and a tick source.
#pragma once
#include "fake_hal.h"
#include <cstdint>

inline USART_TypeDef g_usart{};
inline DMA_Channel_TypeDef g_channel_rx{};
inline DMA_Channel_TypeDef g_channel_tx{};
inline DMA_HandleTypeDef g_dma_rx{};
inline DMA_HandleTypeDef g_dma_tx{};
inline UART_HandleTypeDef huart3{};

inline void configure_huart3(const uint32_t baud) noexcept
{
	huart3.Instance = &g_usart;
	huart3.Init.BaudRate = baud;
	huart3.Init.WordLength = UART_WORDLENGTH_8B;
	huart3.Init.StopBits = UART_STOPBITS_1;
	huart3.Init.Parity = UART_PARITY_NONE;
	huart3.Init.Mode = UART_MODE_TX_RX;
	huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart3.gState = HAL_UART_STATE_READY;
	huart3.RxState = HAL_UART_STATE_READY;
	huart3.ErrorCode = HAL_UART_ERROR_NONE;
	g_dma_rx.Instance = &g_channel_rx;
	g_dma_rx.State = HAL_DMA_STATE_READY;
	g_dma_rx.Parent = &huart3;
	g_dma_rx.Init.Direction = DMA_PERIPH_TO_MEMORY;
	g_dma_rx.Init.PeriphInc = DMA_PINC_DISABLE;
	g_dma_rx.Init.MemInc = DMA_MINC_ENABLE;
	g_dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
	g_dma_rx.Init.MemDataAlignment = DMA_MDATAALIGN_BYTE;
	g_dma_rx.Init.Mode = DMA_NORMAL;
	g_dma_tx = g_dma_rx;
	g_dma_tx.Instance = &g_channel_tx;
	g_dma_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
	huart3.hdmarx = &g_dma_rx;
	huart3.hdmatx = &g_dma_tx;
	fake::model().huart = &huart3;
}

