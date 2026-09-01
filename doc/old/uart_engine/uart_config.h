/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * uartsettingstest.h
 *
 *  Created on: May 21, 2025
 *      Author: admin
 */

#ifndef STM32_TOOLS_UART_ENGINE_UARTSETTINGS_H_
#define STM32_TOOLS_UART_ENGINE_UARTSETTINGS_H_

#if __has_include("uart_settings.h")
#
#include "uart_settings.h"
#
#else
#	define UART_ENGINE_ON 0
#	define UART_ENGINE_RS485_ON 0
#	define UART_ENGINE_INTERNAL_CALLBACKS_ON 0
#endif

/* only for example
 * please copy this to your "uart_settings.h" :

#define UART_ENGINE_ON 1
#define UART_ENGINE_RS485_ON 1
#define UART_ENGINE_INTERNAL_CALLBACKS_ON 1

*/

#endif /* STM32_TOOLS_UART_ENGINE_UARTSETTINGS_H_ */


