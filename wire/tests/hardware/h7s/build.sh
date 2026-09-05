#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT
# Standalone paired endpoint benchmark, against the existing ignored Cube scaffold.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
PROJECT="${H7S_CUBE_PROJECT:-$REPO/stm32_cube_test/h7s_cobs_test}"
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
GCC="$TOOLS/arm-none-eabi-gcc.exe"
GXX="$TOOLS/arm-none-eabi-g++.exe"
OUT="$PROJECT/out/protocol-comparison"
test -x "$GXX"
cmp "$PROJECT/Boot/Core/Inc/uart_bench.h" "$REPO/uart/tests/bench/uart_bench.h"
mkdir -p "$OUT"
MCU="-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb"
INC="-I$PROJECT/Boot/Core/Inc -I$REPO -isystem $REPO/libs/delegate
 -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc
 -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy
 -isystem $PROJECT/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include
 -isystem $PROJECT/Drivers/CMSIS/Include"
FLAGS="$MCU $INC -DUSE_HAL_DRIVER -DSTM32H7S3xx -c -Os
 -ffunction-sections -fdata-sections --specs=nano.specs"
OBJS=""
for f in "$PROJECT"/Drivers/STM32H7RSxx_HAL_Driver/Src/*.c "$PROJECT"/Boot/Core/Src/*.c; do
  o="$OUT/$(basename "$f" .c).o"
  "$GCC" $FLAGS -std=gnu11 -Wall "$f" -o "$o"
  OBJS="$OBJS $o"
done
for f in "$HERE/protocol_bench.cpp" "$REPO/cobs/Decoder.cpp" "$REPO/cobs/Encoder.cpp"; do
  o="$OUT/$(basename "$f" .cpp).o"
  "$GXX" $FLAGS -std=gnu++20 -DPROTOCOL_BENCH_CRC="${PROTOCOL_BENCH_CRC:-1}" \
    -fno-exceptions -fno-rtti -fno-use-cxa-atexit -fstack-usage \
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror "$f" -o "$o"
  OBJS="$OBJS $o"
done
"$GCC" $MCU -c -x assembler-with-cpp --specs=nano.specs \
  "$PROJECT/Boot/Core/Startup/startup_stm32h7s3l8hx.s" -o "$OUT/startup.o"
ELF="$OUT/protocol_bench.elf"
"$GXX" $MCU -o "$ELF" $OBJS "$OUT/startup.o" \
  -T"$PROJECT/Boot/STM32H7S3L8HX_FLASH.ld" --specs=nosys.specs --specs=nano.specs \
  -Wl,-Map="$OUT/protocol_bench.map" -Wl,--gc-sections -static \
  -Wl,--start-group -lc -lm -Wl,--end-group
"$TOOLS/arm-none-eabi-size.exe" "$ELF"
echo "ELF=$ELF"
