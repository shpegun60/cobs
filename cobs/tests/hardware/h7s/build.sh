#!/bin/sh
# Reproducible CLI build of the end-to-end COBS/UART harness against the local
# Cube-generated NUCLEO-H7S3L8 scaffold. Output stays in the ignored Cube tree.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../../.." && pwd)"
PROJECT="${H7S_CUBE_PROJECT:-$REPO/stm32_cube_test/h7s_cobs_test}"
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
GCC="$TOOLS/arm-none-eabi-gcc.exe"
GXX="$TOOLS/arm-none-eabi-g++.exe"
SIZE="$TOOLS/arm-none-eabi-size.exe"
OUT="$PROJECT/out/cobs-hardware"

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
LIB_INC="-I$REPO/uart -I$REPO/cobs \
  -isystem $REPO/libs/spsc -isystem $REPO/libs/spsc/src \
  -isystem $REPO/libs/delegate"
CFLAGS="$MCU -std=gnu11 -DUSE_HAL_DRIVER -DSTM32H7S3xx -c \
  $CORE_INC $HAL_INC -Os -ffunction-sections -fdata-sections \
  -Wall --specs=nano.specs"
CXXFLAGS="$MCU -std=gnu++20 -DUSE_HAL_DRIVER -DSTM32H7S3xx -c \
  $CORE_INC $HAL_INC $LIB_INC -Os -ffunction-sections -fdata-sections \
  -fno-exceptions -fno-rtti -fno-use-cxa-atexit \
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror \
  --specs=nano.specs"

OBJS=""
for f in "$PROJECT"/Drivers/STM32H7RSxx_HAL_Driver/Src/*.c \
         "$PROJECT"/Boot/Core/Src/*.c; do
  o="$OUT/$(basename "$f" .c).o"
  echo "CC  $(basename "$f")"
  "$GCC" $CFLAGS "$f" -o "$o"
  OBJS="$OBJS $o"
done

echo "CXX cobs_bench.cpp"
"$GXX" $CXXFLAGS -DCOBS_HW_BAUD="${COBS_HW_BAUD:-115200}u" \
  "$HERE/cobs_bench.cpp" -o "$OUT/cobs_bench.o"
OBJS="$OBJS $OUT/cobs_bench.o"

for f in "$REPO/cobs/Decoder.cpp" "$REPO/cobs/Encoder.cpp"; do
  o="$OUT/$(basename "$f" .cpp).o"
  echo "CXX $(basename "$f")"
  "$GXX" $CXXFLAGS "$f" -o "$o"
  OBJS="$OBJS $o"
done

echo "AS  startup"
"$GCC" $MCU -c -x assembler-with-cpp --specs=nano.specs \
  "$PROJECT/Boot/Core/Startup/startup_stm32h7s3l8hx.s" -o "$OUT/startup.o"
OBJS="$OBJS $OUT/startup.o"

ELF="$OUT/cobs_hardware_bench.elf"
echo "LD  $(basename "$ELF")"
"$GXX" -o "$ELF" $OBJS $MCU \
  -T"$PROJECT/Boot/STM32H7S3L8HX_FLASH.ld" \
  --specs=nosys.specs --specs=nano.specs \
  -Wl,-Map="$OUT/cobs_hardware_bench.map" -Wl,--gc-sections -static \
  -Wl,--start-group -lc -lm -Wl,--end-group

"$SIZE" "$ELF"
echo "ELF=$ELF"
echo "=== COBS hardware build OK ==="
