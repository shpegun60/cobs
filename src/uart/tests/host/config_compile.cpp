/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/* Compile-only probe used by run.sh to verify configuration diagnostics. */
#include "Uart.h"

#ifndef UART_TEST_CHUNK_SIZE
#define UART_TEST_CHUNK_SIZE 128
#endif
#ifndef UART_TEST_CHUNK_COUNT
#define UART_TEST_CHUNK_COUNT 8
#endif

Uart<UART_TEST_CHUNK_SIZE, UART_TEST_CHUNK_COUNT> g_uart_config_probe;
