/*
 * Host stand-in for the CubeMX "main.h": just enough of the STM32 HAL surface
 * for uart/Uart.h to compile and RUN on a desktop. Types and semantics mirror
 * the real F1/G4/H7RS drivers (new USART IP: ISR/RDR/ICR); the behaviour lives
 * in fake_hal.cpp, which models the HAL's documented quirks rather than an
 * idealised version of them.
 */
#ifndef FAKE_MAIN_H_
#define FAKE_MAIN_H_

#include <cstdint>
#include <cstddef>

#define HAL_UART_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED

typedef enum { HAL_OK = 0, HAL_ERROR = 1, HAL_BUSY = 2, HAL_TIMEOUT = 3 } HAL_StatusTypeDef;

/* ------------------------------ USART regs ------------------------------ */
typedef struct {
	volatile uint32_t CR1, CR2, CR3, BRR, GTPR, RTOR, RQR, ISR, ICR, RDR, TDR;
} USART_TypeDef;

/* Bit definitions the driver and uart_regs.h look for. The presence of
 * USART_RDR_RDR_Msk + USART_ISR_PE_Msk selects the new-IP register map. */
#define USART_RDR_RDR_Msk   (0x1FFUL)
#define USART_ISR_PE_Msk    (0x1UL)
#define USART_CR3_DMAR      (1UL << 6)
#define USART_CR3_DMAT      (1UL << 7)
#define USART_CR3_HDSEL     (1UL << 3)
#define USART_CR3_CTSE      (1UL << 9)
#define USART_CR3_RTSE      (1UL << 8)

#define UART_CLEAR_PEF      (1UL << 0)
#define UART_CLEAR_FEF      (1UL << 1)
#define UART_CLEAR_NEF      (1UL << 2)
#define UART_CLEAR_OREF     (1UL << 3)
#define UART_CLEAR_IDLEF    (1UL << 4)

#define READ_BIT(REG, BIT)  ((REG) & (BIT))

/* ------------------------------- HAL enums ------------------------------ */
typedef uint32_t HAL_UART_StateTypeDef;
#define HAL_UART_STATE_RESET      0x00000000U
#define HAL_UART_STATE_READY      0x00000020U
#define HAL_UART_STATE_BUSY       0x00000024U
#define HAL_UART_STATE_BUSY_TX    0x00000021U
#define HAL_UART_STATE_BUSY_RX    0x00000022U
#define HAL_UART_STATE_BUSY_TX_RX 0x00000023U
#define HAL_UART_STATE_TIMEOUT    0x000000A0U
#define HAL_UART_STATE_ERROR      0x000000E0U

#define HAL_UART_ERROR_NONE 0x00000000U
#define HAL_UART_ERROR_PE   0x00000001U
#define HAL_UART_ERROR_NE   0x00000002U
#define HAL_UART_ERROR_FE   0x00000004U
#define HAL_UART_ERROR_ORE  0x00000008U
#define HAL_UART_ERROR_DMA  0x00000010U
#define HAL_UART_ERROR_RTO  0x00000020U

typedef enum { HAL_DMA_STATE_RESET = 0, HAL_DMA_STATE_READY, HAL_DMA_STATE_BUSY,
               HAL_DMA_STATE_TIMEOUT, HAL_DMA_STATE_ABORT } HAL_DMA_StateTypeDef;
#define HAL_DMA_ERROR_NONE    0x00000000U
#define HAL_DMA_ERROR_TIMEOUT 0x00000020U

typedef enum { HAL_UART_RXEVENT_TC = 0, HAL_UART_RXEVENT_HT = 1,
               HAL_UART_RXEVENT_IDLE = 2 } HAL_UART_RxEventTypeTypeDef;

/* UART framing / mode constants */
#define UART_WORDLENGTH_8B  0x00000000U
#define UART_WORDLENGTH_9B  0x00001000U
#define UART_WORDLENGTH_7B  0x10000000U
#define UART_STOPBITS_1     0x00000000U
#define UART_STOPBITS_2     0x00002000U
#define UART_STOPBITS_1_5   0x00003000U
#define UART_PARITY_NONE    0x00000000U
#define UART_PARITY_EVEN    0x00000400U
#define UART_MODE_TX_RX     0x0000000CU
#define UART_MODE_RX        0x00000004U
#define UART_HWCONTROL_NONE    0x00000000U
#define UART_HWCONTROL_CTS     0x00000200U
#define UART_HWCONTROL_RTS_CTS 0x00000300U

/* Classic-DMA geometry constants (this fake models the classic register map) */
#define DMA_NORMAL             0x00000000U
#define DMA_CIRCULAR           0x00000020U
#define DMA_PERIPH_TO_MEMORY   0x00000000U
#define DMA_MEMORY_TO_PERIPH   0x00000010U
#define DMA_PINC_DISABLE       0x00000000U
#define DMA_PINC_ENABLE        0x00000040U
#define DMA_MINC_ENABLE        0x00000080U
#define DMA_MINC_DISABLE       0x00000000U
#define DMA_PDATAALIGN_BYTE    0x00000000U
#define DMA_PDATAALIGN_HALFWORD 0x00000100U
#define DMA_MDATAALIGN_BYTE    0x00000000U
#define DMA_MDATAALIGN_HALFWORD 0x00000400U
#define DMA_IT_HT              0x00000004U

/* ------------------------------- handles -------------------------------- */
typedef struct { uint32_t dummy; } DMA_Channel_TypeDef;

typedef struct {
	uint32_t Direction, PeriphInc, MemInc, PeriphDataAlignment,
	         MemDataAlignment, Mode, Priority;
} DMA_InitTypeDef;

typedef struct DMA_HandleTypeDef {
	DMA_Channel_TypeDef* Instance;
	DMA_InitTypeDef Init;
	HAL_DMA_StateTypeDef State;
	void* Parent;
	uint32_t ErrorCode;
	uint32_t CountRemaining; // stands in for CNDTR
} DMA_HandleTypeDef;

typedef struct {
	uint32_t BaudRate, WordLength, StopBits, Parity, Mode, HwFlowCtl, OverSampling;
} UART_InitTypeDef;

typedef struct __UART_HandleTypeDef {
	USART_TypeDef* Instance;
	UART_InitTypeDef Init;
	DMA_HandleTypeDef* hdmarx;
	DMA_HandleTypeDef* hdmatx;
	HAL_UART_StateTypeDef gState;
	HAL_UART_StateTypeDef RxState;
	uint32_t ErrorCode;
	HAL_UART_RxEventTypeTypeDef RxEventType;
	uint16_t RxXferSize;
} UART_HandleTypeDef;

/* --------------------------------- API ---------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_UARTEx_ReceiveToIdle_DMA(UART_HandleTypeDef*, uint8_t*, uint16_t);
HAL_StatusTypeDef HAL_UART_Transmit_DMA(UART_HandleTypeDef*, const uint8_t*, uint16_t);
HAL_StatusTypeDef HAL_UART_AbortReceive(UART_HandleTypeDef*);
HAL_StatusTypeDef HAL_UART_AbortTransmit(UART_HandleTypeDef*);
HAL_StatusTypeDef HAL_UART_Abort(UART_HandleTypeDef*);
HAL_StatusTypeDef HAL_UART_DMAStop(UART_HandleTypeDef*);
uint32_t HAL_UART_GetError(const UART_HandleTypeDef*);
uint32_t HAL_DMA_GetError(const DMA_HandleTypeDef*);
HAL_UART_RxEventTypeTypeDef HAL_UARTEx_GetRxEventType(const UART_HandleTypeDef*);

/* callbacks the driver defines under UART_ENGINE_IMPLEMENT */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef*, uint16_t);
void HAL_UART_TxCpltCallback(UART_HandleTypeDef*);
void HAL_UART_ErrorCallback(UART_HandleTypeDef*);

#ifdef __cplusplus
}
#endif

#define __HAL_DMA_GET_COUNTER(h)        ((h)->CountRemaining)
#define __HAL_DMA_DISABLE_IT(h, it)     ((void)(h), (void)(it))
#define __HAL_UART_CLEAR_FLAG(h, f)     ((h)->Instance->ICR = (f))

#endif /* FAKE_MAIN_H_ */
