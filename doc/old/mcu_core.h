/*
 * Author: shpegun60
 * SPDX-License-Identifier: MIT
 */

/*
 * mcu_core.h
 *
 *  Created on: Sep 15, 2025
 *      Author: admin
 */
#pragma once
#ifndef STM32_TOOLS_MCU_CORE_H_
#define STM32_TOOLS_MCU_CORE_H_

/*
 * Strategy:
 * 1. Trust Cube first: main.h should normally pull the HAL umbrella and the
 *    CMSIS device header selected by the project.
 * 2. If core macros are still missing, try a controlled fallback:
 *    - manual override via MCU_CORE_HAL_HEADER / MCU_CORE_DEVICE_HEADER
 *    - CMSIS_device_header if the project defines it
 *    - family inference from STM32 target macros
 * 3. If none of that works, fail with an explicit message telling the user how
 *    to wire the target manually.
 */

#include "main.h"
#include "basic_types.h"

/* __has_include polyfill for older toolchains */
#ifndef MCU_HAS_INCLUDE
#if defined(__has_include)
#define MCU_HAS_INCLUDE(x) __has_include(x)
#else
#define MCU_HAS_INCLUDE(x) 0
#endif
#endif /* MCU_HAS_INCLUDE */

/*
 * Optional manual override hooks.
 *
 * Example:
 *   #define MCU_CORE_HAL_HEADER    "stm32h7xx_hal.h"
 *   #define MCU_CORE_DEVICE_HEADER "stm32h753xx.h"
 *
 * Prefer MCU_CORE_HAL_HEADER when the project expects HAL handle types.
 */

/* Family detection helpers */
#if defined(STM32C0xx) || defined(STM32C011xx) || defined(STM32C031xx)
#define MCU_STM32_FAMILY_C0 1
#else
#define MCU_STM32_FAMILY_C0 0
#endif

#if defined(STM32C5xx) || defined(STM32C531xx) || defined(STM32C53xxx) || \
    defined(STM32C551xx) || defined(STM32C552xx) || defined(STM32C55xxx) || \
    defined(STM32C562xx) || defined(STM32C591xx) || defined(STM32C59xxx) || \
    defined(STM32C5A3xx) || defined(STM32C5A3xxx)
#define MCU_STM32_FAMILY_C5 1
#else
#define MCU_STM32_FAMILY_C5 0
#endif

#if defined(STM32G0xx) || defined(STM32G030xx) || defined(STM32G031xx) || \
    defined(STM32G041xx) || defined(STM32G050xx) || defined(STM32G051xx) || \
    defined(STM32G061xx) || defined(STM32G070xx) || defined(STM32G071xx) || \
    defined(STM32G081xx) || defined(STM32G0B1xx) || defined(STM32G0C1xx)
#define MCU_STM32_FAMILY_G0 1
#else
#define MCU_STM32_FAMILY_G0 0
#endif

#if defined(STM32G4xx) || defined(STM32G431xx) || defined(STM32G441xx) || \
    defined(STM32G471xx) || defined(STM32G473xx) || defined(STM32G474xx) || \
    defined(STM32G483xx) || defined(STM32G484xx)
#define MCU_STM32_FAMILY_G4 1
#else
#define MCU_STM32_FAMILY_G4 0
#endif

#if defined(STM32F0xx) || defined(STM32F030x6) || defined(STM32F030x8) || \
    defined(STM32F030xC) || defined(STM32F031x6) || defined(STM32F038xx) || \
    defined(STM32F042x6) || defined(STM32F048xx) || defined(STM32F051x8) || \
    defined(STM32F058xx) || defined(STM32F070x6) || defined(STM32F070xB) || \
    defined(STM32F071xB) || defined(STM32F072x8) || defined(STM32F072xB) || \
    defined(STM32F078xx) || defined(STM32F091xC) || defined(STM32F098xx)
#define MCU_STM32_FAMILY_F0 1
#else
#define MCU_STM32_FAMILY_F0 0
#endif

#if defined(STM32F1xx) || defined(STM32F100xB) || defined(STM32F100xE) || \
    defined(STM32F101x6) || defined(STM32F101xB) || defined(STM32F101xE) || \
    defined(STM32F101xG) || defined(STM32F102x6) || defined(STM32F102xB) || \
    defined(STM32F103x6) || defined(STM32F103xB) || defined(STM32F103xE) || \
    defined(STM32F103xG) || defined(STM32F105xC) || defined(STM32F107xC)
#define MCU_STM32_FAMILY_F1 1
#else
#define MCU_STM32_FAMILY_F1 0
#endif

#if defined(STM32F2xx) || defined(STM32F205xx) || defined(STM32F207xx) || \
    defined(STM32F215xx) || defined(STM32F217xx)
#define MCU_STM32_FAMILY_F2 1
#else
#define MCU_STM32_FAMILY_F2 0
#endif

#if defined(STM32F3xx) || defined(STM32F301x8) || defined(STM32F302x8) || \
    defined(STM32F302xC) || defined(STM32F302xE) || defined(STM32F303x8) || \
    defined(STM32F303xC) || defined(STM32F303xE) || defined(STM32F318xx) || \
    defined(STM32F328xx) || defined(STM32F334x8) || defined(STM32F334xC) || \
    defined(STM32F358xx) || defined(STM32F373xC) || defined(STM32F378xx) || \
    defined(STM32F398xx)
#define MCU_STM32_FAMILY_F3 1
#else
#define MCU_STM32_FAMILY_F3 0
#endif

#if defined(STM32F4xx) || defined(STM32F401xC) || defined(STM32F401xE) || \
    defined(STM32F405xx) || defined(STM32F407xx) || defined(STM32F410Tx) || \
    defined(STM32F410Cx) || defined(STM32F410Rx) || defined(STM32F411xE) || \
    defined(STM32F412Cx) || defined(STM32F412Rx) || defined(STM32F412Vx) || \
    defined(STM32F412Zx) || defined(STM32F413xx) || defined(STM32F423xx) || \
    defined(STM32F415xx) || defined(STM32F417xx) || defined(STM32F427xx) || \
    defined(STM32F429xx) || defined(STM32F437xx) || defined(STM32F439xx) || \
    defined(STM32F446xx) || defined(STM32F469xx) || defined(STM32F479xx)
#define MCU_STM32_FAMILY_F4 1
#else
#define MCU_STM32_FAMILY_F4 0
#endif

#if defined(STM32F7xx) || defined(STM32F722xx) || defined(STM32F723xx) || \
    defined(STM32F730xx) || defined(STM32F732xx) || defined(STM32F733xx) || \
    defined(STM32F745xx) || defined(STM32F746xx) || defined(STM32F756xx) || \
    defined(STM32F765xx) || defined(STM32F767xx) || defined(STM32F769xx) || \
    defined(STM32F777xx) || defined(STM32F778xx) || defined(STM32F779xx)
#define MCU_STM32_FAMILY_F7 1
#else
#define MCU_STM32_FAMILY_F7 0
#endif

#if defined(STM32H5xx) || defined(STM32H503xx) || defined(STM32H562xx) || \
    defined(STM32H563xx) || defined(STM32H573xx)
#define MCU_STM32_FAMILY_H5 1
#else
#define MCU_STM32_FAMILY_H5 0
#endif

#if defined(STM32H7xx) || defined(STM32H742xx) || defined(STM32H743xx) || \
    defined(STM32H745xx) || defined(STM32H745xG) || defined(STM32H747xx) || \
    defined(STM32H747xG) || defined(STM32H750xx) || defined(STM32H753xx) || \
    defined(STM32H755xx) || defined(STM32H757xx) || defined(STM32H7A3xx) || \
    defined(STM32H7A3xxQ) || defined(STM32H7B0xx) || defined(STM32H7B0xxQ) || \
    defined(STM32H7B3xx) || defined(STM32H7B3xxQ) || defined(STM32H723xx) || \
    defined(STM32H725xx) || defined(STM32H730xx) || defined(STM32H730xxQ) || \
    defined(STM32H733xx) || defined(STM32H735xx)
#define MCU_STM32_FAMILY_H7 1
#else
#define MCU_STM32_FAMILY_H7 0
#endif

#if defined(STM32H7RSxx) || defined(STM32H7R3xx) || defined(STM32H7S3xx) || \
    defined(STM32H7R7xx) || defined(STM32H7S7xx)
#define MCU_STM32_FAMILY_H7RS 1
#else
#define MCU_STM32_FAMILY_H7RS 0
#endif

#if defined(STM32L0xx) || defined(STM32L010x4) || defined(STM32L010x6) || \
    defined(STM32L010x8) || defined(STM32L010xB) || defined(STM32L011xx) || \
    defined(STM32L021xx) || defined(STM32L031xx) || defined(STM32L041xx) || \
    defined(STM32L051xx) || defined(STM32L052xx) || defined(STM32L053xx) || \
    defined(STM32L062xx) || defined(STM32L063xx) || defined(STM32L071xx) || \
    defined(STM32L072xx) || defined(STM32L073xx) || defined(STM32L081xx) || \
    defined(STM32L082xx) || defined(STM32L083xx)
#define MCU_STM32_FAMILY_L0 1
#else
#define MCU_STM32_FAMILY_L0 0
#endif

#if defined(STM32L1xx) || defined(STM32L100xx) || defined(STM32L151xB) || \
    defined(STM32L151xBA) || defined(STM32L151xC) || defined(STM32L151xCA) || \
    defined(STM32L151xD) || defined(STM32L151xDX) || defined(STM32L151xE) || \
    defined(STM32L151xEX) || defined(STM32L152xB) || defined(STM32L152xBA) || \
    defined(STM32L152xC) || defined(STM32L152xCA) || defined(STM32L152xD) || \
    defined(STM32L152xDX) || defined(STM32L152xE) || defined(STM32L152xEX) || \
    defined(STM32L162xx)
#define MCU_STM32_FAMILY_L1 1
#else
#define MCU_STM32_FAMILY_L1 0
#endif

#if defined(STM32L4xx) || defined(STM32L412xx) || defined(STM32L422xx) || \
    defined(STM32L431xx) || defined(STM32L432xx) || defined(STM32L433xx) || \
    defined(STM32L442xx) || defined(STM32L443xx) || defined(STM32L451xx) || \
    defined(STM32L452xx) || defined(STM32L462xx) || defined(STM32L471xx) || \
    defined(STM32L475xx) || defined(STM32L476xx) || defined(STM32L485xx) || \
    defined(STM32L486xx) || defined(STM32L496xx) || defined(STM32L4A6xx) || \
    defined(STM32L4P5xx) || defined(STM32L4Q5xx) || defined(STM32L4R5xx) || \
    defined(STM32L4R7xx) || defined(STM32L4R9xx) || defined(STM32L4S5xx) || \
    defined(STM32L4S7xx) || defined(STM32L4S9xx)
#define MCU_STM32_FAMILY_L4 1
#else
#define MCU_STM32_FAMILY_L4 0
#endif

#if defined(STM32L5xx) || defined(STM32L552xx) || defined(STM32L562xx)
#define MCU_STM32_FAMILY_L5 1
#else
#define MCU_STM32_FAMILY_L5 0
#endif

#if defined(STM32N6xx) || defined(STM32N645xx) || defined(STM32N647xx) || \
    defined(STM32N655xx) || defined(STM32N657xx) || defined(STM32N6x5xx) || \
    defined(STM32N6x7xx)
#define MCU_STM32_FAMILY_N6 1
#else
#define MCU_STM32_FAMILY_N6 0
#endif

#if defined(STM32U0xx) || defined(STM32U031xx) || defined(STM32U073xx) || \
    defined(STM32U083xx)
#define MCU_STM32_FAMILY_U0 1
#else
#define MCU_STM32_FAMILY_U0 0
#endif

#if defined(STM32U3xx) || defined(STM32U3B5xx) || defined(STM32U3C5xx)
#define MCU_STM32_FAMILY_U3 1
#else
#define MCU_STM32_FAMILY_U3 0
#endif

#if defined(STM32U5xx) || defined(STM32U535xx) || defined(STM32U545xx) || \
    defined(STM32U575xx) || defined(STM32U585xx) || defined(STM32U595xx) || \
    defined(STM32U599xx) || defined(STM32U5A5xx) || defined(STM32U5A9xx)
#define MCU_STM32_FAMILY_U5 1
#else
#define MCU_STM32_FAMILY_U5 0
#endif

#if defined(STM32WB0x) || defined(STM32WB05xx) || defined(STM32WB06xx) || \
    defined(STM32WB07xx) || defined(STM32WB09xx)
#define MCU_STM32_FAMILY_WB0 1
#else
#define MCU_STM32_FAMILY_WB0 0
#endif

#if defined(STM32WBxx) || defined(STM32WB10xx) || defined(STM32WB15xx) || \
    defined(STM32WB30xx) || defined(STM32WB35xx) || defined(STM32WB50xx) || \
    defined(STM32WB55xx) || defined(STM32WB5Mxx)
#define MCU_STM32_FAMILY_WB 1
#else
#define MCU_STM32_FAMILY_WB 0
#endif

#if defined(STM32WBAxx) || defined(STM32WBA50xx) || defined(STM32WBA52xx) || \
    defined(STM32WBA54xx) || defined(STM32WBA55xx)
#define MCU_STM32_FAMILY_WBA 1
#else
#define MCU_STM32_FAMILY_WBA 0
#endif

#if defined(STM32WLxx) || defined(STM32WL54xx) || defined(STM32WL55xx) || \
    defined(STM32WLE4xx) || defined(STM32WLE5xx)
#define MCU_STM32_FAMILY_WL 1
#else
#define MCU_STM32_FAMILY_WL 0
#endif

#if defined(STM32WL3x) || defined(STM32WL31xx) || defined(STM32WL33xx)
#define MCU_STM32_FAMILY_WL3 1
#else
#define MCU_STM32_FAMILY_WL3 0
#endif

#if ((MCU_STM32_FAMILY_C0 + MCU_STM32_FAMILY_C5 + MCU_STM32_FAMILY_G0 + \
      MCU_STM32_FAMILY_G4 + MCU_STM32_FAMILY_F0 + MCU_STM32_FAMILY_F1 + \
      MCU_STM32_FAMILY_F2 + MCU_STM32_FAMILY_F3 + MCU_STM32_FAMILY_F4 + \
      MCU_STM32_FAMILY_F7 + MCU_STM32_FAMILY_H5 + MCU_STM32_FAMILY_H7 + \
      MCU_STM32_FAMILY_H7RS + MCU_STM32_FAMILY_L0 + MCU_STM32_FAMILY_L1 + \
      MCU_STM32_FAMILY_L4 + MCU_STM32_FAMILY_L5 + MCU_STM32_FAMILY_N6 + \
      MCU_STM32_FAMILY_U0 + MCU_STM32_FAMILY_U3 + MCU_STM32_FAMILY_U5 + \
      MCU_STM32_FAMILY_WB0 + MCU_STM32_FAMILY_WB + MCU_STM32_FAMILY_WBA + \
      MCU_STM32_FAMILY_WL + MCU_STM32_FAMILY_WL3) > 1)
#error "[mcu]: Multiple STM32 families detected at once. Check your target macros or define MCU_CORE_HAL_HEADER / MCU_CORE_DEVICE_HEADER explicitly."
#endif

/* Recover the HAL/device header chain only if Cube did not expose core macros. */
#if !defined(__CORTEX_M) || !defined(__FPU_PRESENT)

#if defined(MCU_CORE_HAL_HEADER) && defined(MCU_CORE_DEVICE_HEADER)
#error "[mcu]: Define only one of MCU_CORE_HAL_HEADER or MCU_CORE_DEVICE_HEADER."
#endif

#if defined(MCU_CORE_HAL_HEADER)
#include MCU_CORE_HAL_HEADER

#elif defined(MCU_CORE_DEVICE_HEADER)
#include MCU_CORE_DEVICE_HEADER

#elif defined(CMSIS_device_header)
#include CMSIS_device_header

#elif MCU_STM32_FAMILY_C5
#if MCU_HAS_INCLUDE("stm32c5xx_hal.h")
#include "stm32c5xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32c5xx.h")
#include "stm32c5xx.h"
#else
#error "[mcu]: STM32C5 target selected but stm32c5xx headers were not found. Define MCU_CORE_HAL_HEADER or MCU_CORE_DEVICE_HEADER manually."
#endif

#elif MCU_STM32_FAMILY_C0
#if MCU_HAS_INCLUDE("stm32c0xx_hal.h")
#include "stm32c0xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32c0xx.h")
#include "stm32c0xx.h"
#else
#error "[mcu]: STM32C0 target selected but stm32c0xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_G0
#if MCU_HAS_INCLUDE("stm32g0xx_hal.h")
#include "stm32g0xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32g0xx.h")
#include "stm32g0xx.h"
#else
#error "[mcu]: STM32G0 target selected but stm32g0xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_G4
#if MCU_HAS_INCLUDE("stm32g4xx_hal.h")
#include "stm32g4xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32g4xx.h")
#include "stm32g4xx.h"
#else
#error "[mcu]: STM32G4 target selected but stm32g4xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_F0
#if MCU_HAS_INCLUDE("stm32f0xx_hal.h")
#include "stm32f0xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32f0xx.h")
#include "stm32f0xx.h"
#else
#error "[mcu]: STM32F0 target selected but stm32f0xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_F1
#if MCU_HAS_INCLUDE("stm32f1xx_hal.h")
#include "stm32f1xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32f1xx.h")
#include "stm32f1xx.h"
#else
#error "[mcu]: STM32F1 target selected but stm32f1xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_F2
#if MCU_HAS_INCLUDE("stm32f2xx_hal.h")
#include "stm32f2xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32f2xx.h")
#include "stm32f2xx.h"
#else
#error "[mcu]: STM32F2 target selected but stm32f2xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_F3
#if MCU_HAS_INCLUDE("stm32f3xx_hal.h")
#include "stm32f3xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32f3xx.h")
#include "stm32f3xx.h"
#else
#error "[mcu]: STM32F3 target selected but stm32f3xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_F4
#if MCU_HAS_INCLUDE("stm32f4xx_hal.h")
#include "stm32f4xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32f4xx.h")
#include "stm32f4xx.h"
#else
#error "[mcu]: STM32F4 target selected but stm32f4xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_F7
#if MCU_HAS_INCLUDE("stm32f7xx_hal.h")
#include "stm32f7xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32f7xx.h")
#include "stm32f7xx.h"
#else
#error "[mcu]: STM32F7 target selected but stm32f7xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_H5
#if MCU_HAS_INCLUDE("stm32h5xx_hal.h")
#include "stm32h5xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32h5xx.h")
#include "stm32h5xx.h"
#else
#error "[mcu]: STM32H5 target selected but stm32h5xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_H7
#if MCU_HAS_INCLUDE("stm32h7xx_hal.h")
#include "stm32h7xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32h7xx.h")
#include "stm32h7xx.h"
#else
#error "[mcu]: STM32H7 target selected but stm32h7xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_H7RS
#if MCU_HAS_INCLUDE("stm32h7rsxx_hal.h")
#include "stm32h7rsxx_hal.h"
#elif MCU_HAS_INCLUDE("stm32h7rsxx.h")
#include "stm32h7rsxx.h"
#else
#error "[mcu]: STM32H7RS target selected but stm32h7rsxx headers were not found."
#endif

#elif MCU_STM32_FAMILY_L0
#if MCU_HAS_INCLUDE("stm32l0xx_hal.h")
#include "stm32l0xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32l0xx.h")
#include "stm32l0xx.h"
#else
#error "[mcu]: STM32L0 target selected but stm32l0xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_L1
#if MCU_HAS_INCLUDE("stm32l1xx_hal.h")
#include "stm32l1xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32l1xx.h")
#include "stm32l1xx.h"
#else
#error "[mcu]: STM32L1 target selected but stm32l1xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_L4
#if MCU_HAS_INCLUDE("stm32l4xx_hal.h")
#include "stm32l4xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32l4xx.h")
#include "stm32l4xx.h"
#else
#error "[mcu]: STM32L4 target selected but stm32l4xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_L5
#if MCU_HAS_INCLUDE("stm32l5xx_hal.h")
#include "stm32l5xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32l5xx.h")
#include "stm32l5xx.h"
#else
#error "[mcu]: STM32L5 target selected but stm32l5xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_N6
#if MCU_HAS_INCLUDE("stm32n6xx_hal.h")
#include "stm32n6xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32n6xx.h")
#include "stm32n6xx.h"
#else
#error "[mcu]: STM32N6 target selected but stm32n6xx headers were not found. Define MCU_CORE_HAL_HEADER or MCU_CORE_DEVICE_HEADER manually."
#endif

#elif MCU_STM32_FAMILY_U0
#if MCU_HAS_INCLUDE("stm32u0xx_hal.h")
#include "stm32u0xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32u0xx.h")
#include "stm32u0xx.h"
#else
#error "[mcu]: STM32U0 target selected but stm32u0xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_U3
#if MCU_HAS_INCLUDE("stm32u3xx_hal.h")
#include "stm32u3xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32u3xx.h")
#include "stm32u3xx.h"
#else
#error "[mcu]: STM32U3 target selected but stm32u3xx headers were not found. Define MCU_CORE_HAL_HEADER or MCU_CORE_DEVICE_HEADER manually."
#endif

#elif MCU_STM32_FAMILY_U5
#if MCU_HAS_INCLUDE("stm32u5xx_hal.h")
#include "stm32u5xx_hal.h"
#elif MCU_HAS_INCLUDE("stm32u5xx.h")
#include "stm32u5xx.h"
#else
#error "[mcu]: STM32U5 target selected but stm32u5xx headers were not found."
#endif

#elif MCU_STM32_FAMILY_WB0
#if MCU_HAS_INCLUDE("stm32wb0x_hal.h")
#include "stm32wb0x_hal.h"
#elif MCU_HAS_INCLUDE("stm32wb0x.h")
#include "stm32wb0x.h"
#else
#error "[mcu]: STM32WB0 target selected but stm32wb0x headers were not found."
#endif

#elif MCU_STM32_FAMILY_WB
#if MCU_HAS_INCLUDE("stm32wbxx_hal.h")
#include "stm32wbxx_hal.h"
#elif MCU_HAS_INCLUDE("stm32wbxx.h")
#include "stm32wbxx.h"
#else
#error "[mcu]: STM32WB target selected but stm32wbxx headers were not found."
#endif

#elif MCU_STM32_FAMILY_WBA
#if MCU_HAS_INCLUDE("stm32wbaxx_hal.h")
#include "stm32wbaxx_hal.h"
#elif MCU_HAS_INCLUDE("stm32wbaxx.h")
#include "stm32wbaxx.h"
#else
#error "[mcu]: STM32WBA target selected but stm32wbaxx headers were not found."
#endif

#elif MCU_STM32_FAMILY_WL
#if MCU_HAS_INCLUDE("stm32wlxx_hal.h")
#include "stm32wlxx_hal.h"
#elif MCU_HAS_INCLUDE("stm32wlxx.h")
#include "stm32wlxx.h"
#else
#error "[mcu]: STM32WL target selected but stm32wlxx headers were not found."
#endif

#elif MCU_STM32_FAMILY_WL3
#if MCU_HAS_INCLUDE("stm32wl3x_hal.h")
#include "stm32wl3x_hal.h"
#elif MCU_HAS_INCLUDE("stm32wl3x.h")
#include "stm32wl3x.h"
#else
#error "[mcu]: STM32WL3 target selected but stm32wl3x headers were not found. Define MCU_CORE_HAL_HEADER or MCU_CORE_DEVICE_HEADER manually."
#endif

#else
#error "[mcu]: Unable to detect the target automatically. Include Cube main.h, define CMSIS_device_header, or define MCU_CORE_HAL_HEADER / MCU_CORE_DEVICE_HEADER before including mcu_core.h."
#endif

#endif /* !defined(__CORTEX_M) || !defined(__FPU_PRESENT) */

/* CMSIS compiler helpers: some core headers already pull this in, which is fine. */
#if !defined(__STATIC_INLINE) || !defined(__ASM)
#include "cmsis_compiler.h"
#endif

#if !defined(__STATIC_INLINE) || !defined(__ASM)
#error "[mcu]: cmsis_compiler.h missing or too old. Add Drivers/CMSIS/Include from a CMSIS 5+ pack."
#endif

#ifndef __CORTEX_M
#error "[mcu]: CMSIS-Core is not visible. Use Cube main.h or define MCU_CORE_HAL_HEADER / MCU_CORE_DEVICE_HEADER."
#endif

/*
 * If HAL is expected by the project, require the HAL umbrella to be visible.
 * HAL_MAX_DELAY comes from stm32xxxx_hal_def.h across STM32 HAL families.
 */
#if defined(USE_HAL_DRIVER) && !defined(HAL_MAX_DELAY)
#error "[mcu]: USE_HAL_DRIVER is defined but HAL headers are not visible. Include Cube main.h or define MCU_CORE_HAL_HEADER."
#endif

/* Cortex-M builds must target Thumb ISA. */
#if !(defined(__thumb__) || defined(__thumb2__) || defined(__TARGET_ARCH_THUMB) || \
      defined(__ARM_ARCH_ISA_THUMB))
#error "[mcu]: Thumb mode required (-mthumb)."
#endif

/* STM32 targets handled here are 32-bit only. */
#if defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ != 4)
#error "[mcu]: Unsupported pointer size (expected 32-bit)."
#endif

#endif /* STM32_TOOLS_MCU_CORE_H_ */
