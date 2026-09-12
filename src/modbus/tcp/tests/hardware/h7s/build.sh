#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do [ "$ROOT" != / ] || exit 1; ROOT="$(dirname "$ROOT")"; done
PROJECT="${H7S_CUBE_PROJECT:-$ROOT/stm32_cube_test/h7s_cobs_test}"
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
OUT="$PROJECT/out/tcp-core"
mkdir -p "$OUT"
cmp "$PROJECT/Boot/Core/Inc/uart_bench.h" "$ROOT/src/uart/tests/bench/uart_bench.h"
MCU="-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb"
INC="-I$PROJECT/Boot/Core/Inc -I$ROOT/src -isystem $ROOT/libs/delegate -isystem $ROOT/libs/spsc -isystem $ROOT/libs/spsc/src -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy -isystem $PROJECT/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include -isystem $PROJECT/Drivers/CMSIS/Include"
FLAGS="$MCU $INC -DUSE_HAL_DRIVER -DSTM32H7S3xx -Os -ffunction-sections -fdata-sections --specs=nano.specs"
OBJS=""
for f in "$PROJECT"/Drivers/STM32H7RSxx_HAL_Driver/Src/*.c "$PROJECT"/Boot/Core/Src/*.c; do
    name="$(basename "$f" .c)"; o="$OUT/$name.o"; extra=""
    if [ "$name" = sysmem ]; then extra="-D_sbrk=cube_sbrk_unused"; fi
    "$TOOLS/arm-none-eabi-gcc.exe" $FLAGS $extra -std=gnu11 -Wall -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
"$TOOLS/arm-none-eabi-g++.exe" $FLAGS -std=gnu++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
    -c "$ROOT/src/wire/tests/hardware/h7s/heap_crc/heap.cpp" -o "$OUT/heap.o"
"$TOOLS/arm-none-eabi-gcc.exe" $MCU -c -x assembler-with-cpp "$PROJECT/Boot/Core/Startup/startup_stm32h7s3l8hx.s" -o "$OUT/startup.o"
for config in 0-0 1-0 0-1 0-2 1-2 0-3; do
    heap="${config%-*}"; crc="${config#*-}"
    dir="$OUT/$config"; mkdir -p "$dir"
    "$TOOLS/arm-none-eabi-g++.exe" $FLAGS -std=gnu++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
        -DTCP_HW_HEAP=$heap -DTCP_HW_CRC=$crc -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror \
        -c "$HERE/bench.cpp" -o "$dir/bench.o"
    "$TOOLS/arm-none-eabi-g++.exe" $MCU $OBJS "$OUT/heap.o" "$OUT/startup.o" "$dir/bench.o" \
        -T"$PROJECT/Boot/STM32H7S3L8HX_FLASH.ld" --specs=nosys.specs --specs=nano.specs \
        -Wl,-Map="$dir/bench.map" -Wl,--gc-sections -static -Wl,--start-group -lc -lm -Wl,--end-group -o "$dir/bench.elf"
    "$TOOLS/arm-none-eabi-size.exe" "$dir/bench.elf"
    "$TOOLS/arm-none-eabi-objcopy.exe" -O binary "$dir/bench.elf" "$dir/bench.bin"
    "$TOOLS/arm-none-eabi-objdump.exe" -d -C "$dir/bench.elf" > "$dir/bench.dis"
    "$TOOLS/arm-none-eabi-nm.exe" -C -S "$dir/bench.elf" > "$dir/bench.nm"
done
