#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
# Compile-only proof of task entries/manual RTU with REAL H7RS HAL + FreeRTOS headers.
# No host fake, linking, firmware image, COM port, flashing or board mutation.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do
    [ "$ROOT" != / ] || { echo 'COBS.pro not found' >&2; exit 1; }
    ROOT="$(dirname "$ROOT")"
done
PROJECT="${H7S_CUBE_PROJECT:-$ROOT/stm32_cube_test/h7s_cobs_test}"
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
KERNEL="${FREERTOS_SOURCE:-/c/Users/admin/STM32Cube/Repository/STM32Cube_FW_H7RS_V1.3.0/Middlewares/Third_Party/FreeRTOS/Source}"
PORT="$KERNEL/portable/GCC/ARM_CM7/r0p1"
OUT="$HERE/out/freertos-arm"
mkdir -p "$OUT"
for file in "$TOOLS/arm-none-eabi-g++.exe" "$PROJECT/Boot/Core/Inc/main.h" "$KERNEL/include/task.h" "$PORT/portmacro.h"; do
    [ -f "$file" ] || { echo "required local dependency missing: $file" >&2; exit 1; }
done
for mode in task-cobs task-rtu manual-rtu manual-rtu-wake; do
    case "$mode" in
        task-cobs) source=freertos_entry.cpp; define=-DDOC_RTU=0 ;;
        task-rtu) source=freertos_entry.cpp; define=-DDOC_RTU=1 ;;
        manual-rtu) source=rtu_uart_direct.cpp; define=-DDOC_WAKE=0 ;;
        manual-rtu-wake) source=rtu_uart_direct.cpp; define=-DDOC_WAKE=1 ;;
    esac
    "$TOOLS/arm-none-eabi-g++.exe" -std=gnu++20 -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard \
        -DUSE_HAL_DRIVER -DSTM32H7S3xx "$define" -Os --specs=nano.specs \
        -fno-exceptions -fno-rtti -fno-use-cxa-atexit -ffunction-sections -fdata-sections \
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror \
        -I"$ROOT/src" -I"$PROJECT/Boot/Core/Inc" \
        -I"$ROOT/src/adapters/tests/hardware/h7s/parity" \
        -isystem "$PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc" \
        -isystem "$PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy" \
        -isystem "$PROJECT/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include" \
        -isystem "$PROJECT/Drivers/CMSIS/Include" -isystem "$KERNEL/include" -isystem "$PORT" \
        -isystem "$ROOT/libs/delegate" -isystem "$ROOT/libs/spsc" -isystem "$ROOT/libs/spsc/src" \
        -c "$HERE/$source" -o "$OUT/$mode.o"
    echo "real FreeRTOS/HAL: $mode compile PASS"
done
