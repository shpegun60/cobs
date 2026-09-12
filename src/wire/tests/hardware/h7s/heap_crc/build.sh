#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do [ "$ROOT" != / ] || exit 1; ROOT="$(dirname "$ROOT")"; done
PROJECT="${H7S_CUBE_PROJECT:-$ROOT/stm32_cube_test/h7s_cobs_test}"
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
PROTOCOL="${HEAP_CRC_PROTOCOL:-0}"
POLICY="${HEAP_CRC_POLICY:-3}"
case "$PROTOCOL:$POLICY" in [01]:[0123]) ;; *) echo "invalid protocol/policy"; exit 1 ;; esac
OUT="$PROJECT/out/heap-crc-$PROTOCOL-$POLICY"
mkdir -p "$OUT"
cmp "$PROJECT/Boot/Core/Inc/uart_bench.h" "$ROOT/src/uart/tests/bench/uart_bench.h"
MCU="-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb"
INC="-I$PROJECT/Boot/Core/Inc -I$ROOT/src -isystem $ROOT/libs/delegate -isystem $ROOT/libs/spsc -isystem $ROOT/libs/spsc/src -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy -isystem $PROJECT/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include -isystem $PROJECT/Drivers/CMSIS/Include"
FLAGS="$MCU $INC -DUSE_HAL_DRIVER -DSTM32H7S3xx -Os -ffunction-sections -fdata-sections --specs=nano.specs"
OBJS=""
for f in "$PROJECT"/Drivers/STM32H7RSxx_HAL_Driver/Src/*.c "$PROJECT"/Boot/Core/Src/*.c; do
    name="$(basename "$f" .c)"; o="$OUT/$name.o"; extra=""
    # Keep generated sysmem.c byte-identical. The benchmark supplies _sbrk
    # over a normal SRAM arena; it does NOT replace malloc/free/operator new.
    if [ "$name" = sysmem ]; then extra="-D_sbrk=cube_sbrk_unused"; fi
    "$TOOLS/arm-none-eabi-gcc.exe" $FLAGS $extra -std=gnu11 -Wall -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
for f in "$HERE/bench.cpp" "$HERE/heap.cpp" "$ROOT/src/cobs/Encoder.cpp" "$ROOT/src/cobs/Decoder.cpp"; do
    o="$OUT/$(basename "$f" .cpp).o"
    "$TOOLS/arm-none-eabi-g++.exe" $FLAGS -std=gnu++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fstack-usage \
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror \
        -DHEAP_CRC_PROTOCOL="$PROTOCOL" -DHEAP_CRC_POLICY="$POLICY" -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
"$TOOLS/arm-none-eabi-gcc.exe" $MCU -c -x assembler-with-cpp "$PROJECT/Boot/Core/Startup/startup_stm32h7s3l8hx.s" -o "$OUT/startup.o"
ELF="$OUT/bench.elf"
"$TOOLS/arm-none-eabi-g++.exe" $MCU $OBJS "$OUT/startup.o" -T"$PROJECT/Boot/STM32H7S3L8HX_FLASH.ld" \
    --specs=nosys.specs --specs=nano.specs -Wl,-Map="$OUT/bench.map" -Wl,--gc-sections -static \
    -Wl,--start-group -lc -lm -Wl,--end-group -o "$ELF"
"$TOOLS/arm-none-eabi-size.exe" "$ELF"
"$TOOLS/arm-none-eabi-objcopy.exe" -O binary "$ELF" "$OUT/bench.bin"
"$TOOLS/arm-none-eabi-objdump.exe" -d -C "$ELF" > "$OUT/bench.dis"
"$TOOLS/arm-none-eabi-nm.exe" -C -S "$ELF" > "$OUT/bench.nm"
echo "ELF=$ELF"
