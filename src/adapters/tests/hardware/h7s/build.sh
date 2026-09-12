#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT
# Separate output: safe to compile while other hardware suites run.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do
    [ "$ROOT" != / ] || exit 1
    ROOT="$(dirname "$ROOT")"
done
PROJECT="${H7S_CUBE_PROJECT:-$ROOT/stm32_cube_test/h7s_cobs_test}"
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
OPT="${AUDIT_OPT:--Os}"
case "$OPT" in -Os|-O2|-O3) ;; *) echo "unsupported AUDIT_OPT=$OPT"; exit 1 ;; esac
OUT="$PROJECT/out/paranoid-hardware${OPT}"
mkdir -p "$OUT"
cmp "$PROJECT/Boot/Core/Inc/uart_bench.h" "$ROOT/src/uart/tests/bench/uart_bench.h"
MCU="-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb"
INC="-I$PROJECT/Boot/Core/Inc -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy -isystem $PROJECT/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include -isystem $PROJECT/Drivers/CMSIS/Include"
COMMON="$MCU -DUSE_HAL_DRIVER -DSTM32H7S3xx $OPT -ffunction-sections -fdata-sections --specs=nano.specs $INC"
OBJS=""
for f in "$PROJECT"/Drivers/STM32H7RSxx_HAL_Driver/Src/*.c "$PROJECT"/Boot/Core/Src/*.c; do
    o="$OUT/$(basename "$f" .c).o"
    "$TOOLS/arm-none-eabi-gcc.exe" $COMMON -std=gnu11 -Wall -c "$f" -o "$o"
    OBJS="$OBJS $o"
done
"$TOOLS/arm-none-eabi-g++.exe" $COMMON -std=gnu++20 -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror \
    -I"$ROOT/src" -isystem "$ROOT/libs/spsc" -isystem "$ROOT/libs/spsc/src" -isystem "$ROOT/libs/delegate" \
    -c "$HERE/audit_bench.cpp" -o "$OUT/audit_bench.o"
"$TOOLS/arm-none-eabi-gcc.exe" $MCU -c -x assembler-with-cpp \
    "$PROJECT/Boot/Core/Startup/startup_stm32h7s3l8hx.s" -o "$OUT/startup.o"
ELF="$OUT/audit_bench.elf"
"$TOOLS/arm-none-eabi-g++.exe" $MCU $OBJS "$OUT/audit_bench.o" "$OUT/startup.o" \
    -T"$PROJECT/Boot/STM32H7S3L8HX_FLASH.ld" --specs=nosys.specs --specs=nano.specs \
    -Wl,-Map="$OUT/audit_bench.map" -Wl,--gc-sections -static -Wl,--start-group -lc -lm -Wl,--end-group -o "$ELF"
"$TOOLS/arm-none-eabi-size.exe" "$ELF"
if "$TOOLS/arm-none-eabi-nm.exe" -C "$ELF" | grep -E 'operator (new|delete)| (malloc|free|_sbrk)$'; then
    echo "unexpected allocator in Pool-only audit image"; exit 1
fi
"$TOOLS/arm-none-eabi-objcopy.exe" -O binary "$ELF" "$OUT/audit_bench.bin"
"$TOOLS/arm-none-eabi-objdump.exe" -d -C "$ELF" > "$OUT/audit_bench.dis"
echo "ELF=$ELF"
