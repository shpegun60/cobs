/* Author: shpegun60; SPDX-License-Identifier: MIT */
#ifndef PARITY_FREERTOS_CONFIG_H_
#define PARITY_FREERTOS_CONFIG_H_

#include <stdint.h>
extern uint32_t SystemCoreClock;
void parity_assert_failed(void);
void parity_trace_notify(void);

#define configUSE_PREEMPTION 1
#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configCPU_CLOCK_HZ SystemCoreClock
#define configTICK_RATE_HZ 1000u
#define configMAX_PRIORITIES 5u
#define configMINIMAL_STACK_SIZE 256u
#define configMAX_TASK_NAME_LEN 16u
#define configUSE_16_BIT_TICKS 0
#define configUSE_IDLE_HOOK 1
#define configUSE_TICK_HOOK 0
#define configUSE_TIMERS 0
#define configUSE_CO_ROUTINES 0
#define configUSE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 0
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_TASK_NOTIFICATIONS 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_NEWLIB_REENTRANT 0
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configQUEUE_REGISTRY_SIZE 0
#define configPRIO_BITS 4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15u
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5u
#define configKERNEL_INTERRUPT_PRIORITY (15u << 4u)
#define configMAX_SYSCALL_INTERRUPT_PRIORITY (5u << 4u)
#define INCLUDE_vTaskDelay 1
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define configASSERT(x) do { if (!(x)) { parity_assert_failed(); } } while (0)
#define traceTASK_NOTIFY_GIVE_FROM_ISR(index) parity_trace_notify()
#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
// SysTick_Handler below maintains both HAL milliseconds and the kernel tick.

#endif
