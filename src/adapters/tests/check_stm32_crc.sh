#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
# Compile/codegen only; this script never accesses the board.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do
    [ "$ROOT" != / ] || { echo "repository root (COBS.pro) not found above $HERE" >&2; exit 1; }
    ROOT="$(dirname "$ROOT")"
done
PROJECT="${H7S_CUBE_PROJECT:-$ROOT/stm32_cube_test/h7s_cobs_test}"
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
OUT="$HERE/out/stm32-crc"
mkdir -p "$OUT"
INC="-I$PROJECT/Boot/Core/Inc -I$ROOT/src -isystem $ROOT/libs/delegate -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy -isystem $PROJECT/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include -isystem $PROJECT/Drivers/CMSIS/Include"
for optimization in s 2 3; do
    for alignment in default strict; do
        extra=""
        if [ "$alignment" = strict ]; then extra="-mno-unaligned-access"; fi
        file="$OUT/O$optimization-$alignment"
        "$TOOLS/arm-none-eabi-g++.exe" -std=gnu++20 -mcpu=cortex-m7 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard \
            -DUSE_HAL_DRIVER -DSTM32H7S3xx $INC -O"$optimization" $extra -fno-exceptions -fno-rtti \
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror \
            -c "$HERE/arm_stm32_crc.cpp" -o "$file.o"
        "$TOOLS/arm-none-eabi-objdump.exe" -drC "$file.o" > "$file.dis"
        "$TOOLS/arm-none-eabi-nm.exe" -CS "$file.o" > "$file.nm"
        if grep -q 'lookup_' "$file.nm"; then echo "unexpected CRC table"; exit 1; fi
        if ! grep -q 'hardware_crc' "$file.nm"; then echo "missing calculation probe"; exit 1; fi
        for instruction in rev str strb; do
            if ! grep -Eq "[[:space:]]$instruction(\\.w)?[[:space:]]" "$file.dis"; then
                echo "missing $instruction in hardware CRC probe: $file"; exit 1
            fi
        done
        if [ "$alignment" = default ] && grep -Eq '[[:space:]](bl|blx)(\.w)?[[:space:]]|R_ARM_THM_(CALL|JUMP24)' "$file.dis"; then
            echo "unexpected helper/dispatch call in default-alignment CRC: $file"; exit 1
        fi
        echo "PASS M7 -O$optimization $alignment policy/API parity; REV + word/byte writes; no table"
    done
done
"$TOOLS/arm-none-eabi-g++.exe" --version | head -n 1
