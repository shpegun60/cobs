#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do [ "$ROOT" != / ] || exit 1; ROOT="$(dirname "$ROOT")"; done
PROJECT="${H7S_CUBE_PROJECT:-$ROOT/stm32_cube_test/h7s_cobs_test}"
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
KERNEL="${FREERTOS_SOURCE:-/c/Users/admin/STM32Cube/Repository/STM32Cube_FW_H7RS_V1.3.0/Middlewares/Third_Party/FreeRTOS/Source}"
PORT="$KERNEL/portable/GCC/ARM_CM7/r0p1"
PROTOCOL="${PARITY_PROTOCOL:-0}"; CRC="${PARITY_CRC:-1}"; BAUD="${PARITY_BAUD:-115200}"
case "$PROTOCOL:$CRC:$BAUD" in [012]:[012]:115200|[012]:[012]:1000000) ;; *) echo "invalid parity configuration"; exit 1 ;; esac
OUT="$PROJECT/out/api-rtos-$PROTOCOL-$CRC-$BAUD"
mkdir -p "$OUT"
cmp "$PROJECT/Boot/Core/Inc/uart_bench.h" "$ROOT/src/uart/tests/bench/uart_bench.h"
MCU="-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb"
INC="-I$PROJECT/Boot/Core/Inc -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy -isystem $PROJECT/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include -isystem $PROJECT/Drivers/CMSIS/Include"
COMMON="$MCU -DUSE_HAL_DRIVER -DSTM32H7S3xx -Os -ffunction-sections -fdata-sections --specs=nano.specs $INC"
KINC="-I$HERE -isystem $KERNEL/include -isystem $PORT"
OBJS=""
for f in "$PROJECT"/Drivers/STM32H7RSxx_HAL_Driver/Src/*.c "$PROJECT"/Boot/Core/Src/*.c; do
    name="$(basename "$f" .c)"; o="$OUT/$name.o"; extra=""
    # Keep Cube sources byte-identical; only this image supplies RTOS handlers.
    if [ "$name" = stm32h7rsxx_it ]; then extra="-DSysTick_Handler=Cube_SysTick_Handler -DSVC_Handler=Cube_SVC_Handler -DPendSV_Handler=Cube_PendSV_Handler"; fi
    "$TOOLS/arm-none-eabi-gcc.exe" $COMMON $extra -std=gnu11 -Wall -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
for f in "$KERNEL/tasks.c" "$KERNEL/list.c" "$KERNEL/queue.c" "$PORT/port.c"; do
    o="$OUT/kernel_$(basename "$f" .c).o"
    "$TOOLS/arm-none-eabi-gcc.exe" $COMMON $KINC -std=gnu11 -Wall -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
for f in "$HERE/parity_bench.cpp" "$ROOT/src/cobs/Encoder.cpp" "$ROOT/src/cobs/Decoder.cpp"; do
    o="$OUT/$(basename "$f" .cpp).o"
    "$TOOLS/arm-none-eabi-g++.exe" $COMMON $KINC -std=gnu++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror \
        -DPARITY_PROTOCOL="$PROTOCOL" -DPARITY_CRC="$CRC" -DPARITY_BAUD="${BAUD}u" \
        -I"$ROOT/src" -isystem "$ROOT/libs/spsc" -isystem "$ROOT/libs/spsc/src" -isystem "$ROOT/libs/delegate" \
        -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
"$TOOLS/arm-none-eabi-gcc.exe" $MCU -c -x assembler-with-cpp "$PROJECT/Boot/Core/Startup/startup_stm32h7s3l8hx.s" -o "$OUT/startup.o"
ELF="$OUT/parity_bench.elf"
"$TOOLS/arm-none-eabi-g++.exe" $MCU $OBJS "$OUT/startup.o" \
    -T"$PROJECT/Boot/STM32H7S3L8HX_FLASH.ld" --specs=nosys.specs --specs=nano.specs \
    -Wl,-Map="$OUT/parity_bench.map" -Wl,--gc-sections -static -Wl,--start-group -lc -lm -Wl,--end-group -o "$ELF"
"$TOOLS/arm-none-eabi-size.exe" "$ELF"
if "$TOOLS/arm-none-eabi-nm.exe" -C "$ELF" | grep -E 'operator (new|delete)| (malloc|free|pvPortMalloc|_sbrk)$'; then echo "unexpected heap allocator"; exit 1; fi
"$TOOLS/arm-none-eabi-objcopy.exe" -O binary "$ELF" "$OUT/parity_bench.bin"
"$TOOLS/arm-none-eabi-objdump.exe" -d -C "$ELF" > "$OUT/parity_bench.dis"
echo "ELF=$ELF"
