#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Reproducible strict ARM build of the Modbus RTU/UART H7S harness. All build
# products stay under the gitignored local Cube scaffold.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../../../../" && pwd)"
PROJECT="${H7S_CUBE_PROJECT:-$REPO/stm32_cube_test/h7s_cobs_test}"
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
GCC="$TOOLS/arm-none-eabi-gcc.exe"
GXX="$TOOLS/arm-none-eabi-g++.exe"
SIZE="$TOOLS/arm-none-eabi-size.exe"
NM="$TOOLS/arm-none-eabi-nm.exe"
OUT="$PROJECT/out/modbus-hardware"
OPT="${MODBUS_HW_OPT:--Os}"
LTO=""
CRC_POLICY="${MODBUS_HW_CRC_POLICY:-bitwise}"
CRC_DEFINE=""

case "$OPT" in
  -Os|-O2|-O3) ;;
  *)
    echo "Unsupported MODBUS_HW_OPT: $OPT (expected -Os, -O2 or -O3)"
    exit 1
    ;;
esac
case "${MODBUS_HW_LTO:-0}" in
  0) ;;
  1) LTO="-flto" ;;
  *)
    echo "Unsupported MODBUS_HW_LTO: ${MODBUS_HW_LTO} (expected 0 or 1)"
    exit 1
    ;;
esac
case "$CRC_POLICY" in
  bitwise) CRC_DEFINE="-DMODBUS_HW_CRC_TABLE=0" ;;
  table) CRC_DEFINE="-DMODBUS_HW_CRC_TABLE=1" ;;
  *)
    echo "Unsupported MODBUS_HW_CRC_POLICY: $CRC_POLICY (expected bitwise or table)"
    exit 1
    ;;
esac

if [ ! -x "$GXX" ]; then
  echo "ARM compiler not found: $GXX"
  exit 1
fi
if [ ! -d "$PROJECT/Boot/Core" ]; then
  echo "Cube project not found: $PROJECT"
  exit 1
fi
if ! cmp -s "$PROJECT/Boot/Core/Inc/uart_bench.h" \
             "$REPO/uart/tests/bench/uart_bench.h"; then
  echo "Cube uart_bench.h differs from the tracked IRQ-counter contract"
  exit 1
fi

mkdir -p "$OUT"

MCU="-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb"
CORE_INC="-I$PROJECT/Boot/Core/Inc"
HAL_INC="-isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc \
  -isystem $PROJECT/Drivers/STM32H7RSxx_HAL_Driver/Inc/Legacy \
  -isystem $PROJECT/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include \
  -isystem $PROJECT/Drivers/CMSIS/Include"
LIB_INC="-I$REPO/uart -I$REPO \
  -isystem $REPO/libs/spsc -isystem $REPO/libs/spsc/src \
  -isystem $REPO/libs/delegate"
CFLAGS="$MCU -std=gnu11 -DUSE_HAL_DRIVER -DSTM32H7S3xx -c \
  $CORE_INC $HAL_INC $OPT $LTO -ffunction-sections -fdata-sections \
  -Wall --specs=nano.specs"
CXXFLAGS="$MCU -std=gnu++20 -DUSE_HAL_DRIVER -DSTM32H7S3xx -c \
  $CORE_INC $HAL_INC $LIB_INC $OPT $LTO -ffunction-sections -fdata-sections \
  -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror \
  --specs=nano.specs"

OBJS=""
echo "CONFIG optimization=$OPT lto=${MODBUS_HW_LTO:-0} baud=${MODBUS_HW_BAUD:-115200} crc=$CRC_POLICY"
for f in "$PROJECT"/Drivers/STM32H7RSxx_HAL_Driver/Src/*.c \
         "$PROJECT"/Boot/Core/Src/*.c; do
  o="$OUT/$(basename "$f" .c).o"
  c_lto_override=""
  case "$(basename "$f")" in
    syscalls.c|sysmem.c)
      # These generated retarget files contain many independent weak hooks,
      # including _sbrk. Keeping their ordinary function sections lets
      # --gc-sections prove and discard every unused heap hook even when the
      # rest of the image participates in LTO.
      c_lto_override="-fno-lto"
      ;;
  esac
  echo "CC  $(basename "$f")"
  "$GCC" $CFLAGS $c_lto_override "$f" -o "$o"
  OBJS="$OBJS $o"
done

echo "CXX modbus_bench.cpp"
"$GXX" $CXXFLAGS -DMODBUS_HW_BAUD="${MODBUS_HW_BAUD:-115200}u" \
  $CRC_DEFINE \
  "$HERE/modbus_bench.cpp" -o "$OUT/modbus_bench.o"
OBJS="$OBJS $OUT/modbus_bench.o"

echo "AS  startup"
"$GCC" $MCU -c -x assembler-with-cpp --specs=nano.specs \
  "$PROJECT/Boot/Core/Startup/startup_stm32h7s3l8hx.s" -o "$OUT/startup.o"
OBJS="$OBJS $OUT/startup.o"

ELF="$OUT/modbus_hardware_bench.elf"
echo "LD  $(basename "$ELF")"
"$GXX" -o "$ELF" $OBJS $MCU \
  $LTO \
  -T"$PROJECT/Boot/STM32H7S3L8HX_FLASH.ld" \
  --specs=nosys.specs --specs=nano.specs \
  -Wl,-Map="$OUT/modbus_hardware_bench.map" -Wl,--gc-sections -static \
  -Wl,--start-group -lc -lm -Wl,--end-group

"$SIZE" "$ELF"
if "$NM" -C "$ELF" | grep -E 'operator (new|delete)| (malloc|free|_sbrk)$'; then
  echo "Pool-only Modbus hardware image unexpectedly linked an allocator"
  exit 1
fi
echo "No allocator symbol linked into the Pool-only image"
echo "ELF=$ELF"
echo "=== Modbus RTU hardware build OK ==="
