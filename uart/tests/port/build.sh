#!/bin/sh
# Portability build matrix for uart/Uart.h (compile-only, no linking).
# Targets: F1 (legacy USART IP, classic DMA, CM3, no cache)
#          G4 (new USART IP, classic DMA, CM4, no cache)
#          H7RS (new USART IP, GPDMA/HPDMA, CM7, D-cache)
# Usage: ./build.sh  (run from any directory; paths are self-anchored)

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../../.." && pwd)"

GCC="/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-g++.exe"
OBJDUMP="${GCC%g++.exe}objdump.exe"
H7RS_FW="/c/Users/admin/STM32Cube/Repository/STM32Cube_FW_H7RS_V1.3.0"
CMSIS_CORE="$H7RS_FW/Drivers/CMSIS/Core/Include"

COMMON_FLAGS="-c -std=gnu++20 -Os -Wall -Wextra -fno-exceptions -fno-rtti \
  -ffunction-sections -fdata-sections -mthumb \
  -I$HERE -I$PROJ/uart -I$PROJ/libs/spsc/src -I$PROJ/libs/delegate -I$CMSIS_CORE"

OUT="$HERE/out"
mkdir -p "$OUT"

echo "=== F1 (STM32F103xE, Cortex-M3, legacy SR/DR) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m3 -mfloat-abi=soft -DSTM32F103xE \
  -I"$HERE/f1" \
  -I"$PROJ/libs/stm32f1xx-hal-driver/Inc" \
  -I"$PROJ/libs/cmsis-device-f1/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_f1.o"
echo "OK"

echo "=== G4 (STM32G474xx, Cortex-M4, new ISR/RDR, classic DMA) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m4 -mfloat-abi=soft -DSTM32G474xx \
  -I"$HERE/g4" \
  -I"$PROJ/libs/stm32g4xx-hal-driver/Inc" \
  -I"$PROJ/libs/cmsis-device-g4/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4.o"
echo "OK"

echo "=== G4 variant: UART_ENGINE_HAS_RXEVENT_TYPE=0 (old-HAL fallback branch) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m4 -mfloat-abi=soft -DSTM32G474xx \
  -DUART_ENGINE_HAS_RXEVENT_TYPE=0 \
  -I"$HERE/g4" \
  -I"$PROJ/libs/stm32g4xx-hal-driver/Inc" \
  -I"$PROJ/libs/cmsis-device-g4/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4_noevt.o"
echo "OK"

echo "=== G4 variant: USE_HAL_UART_REGISTER_CALLBACKS=1 (registered-callbacks init path) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m4 -mfloat-abi=soft -DSTM32G474xx \
  -DUSE_HAL_UART_REGISTER_CALLBACKS=1U \
  -I"$HERE/g4" \
  -I"$PROJ/libs/stm32g4xx-hal-driver/Inc" \
  -I"$PROJ/libs/cmsis-device-g4/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4_regcb.o"
echo "OK"

echo "=== H7RS (STM32H7S3xx, Cortex-M7, D-cache, GPDMA) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m7 -mfloat-abi=soft -DSTM32H7S3xx \
  -I"$HERE/h7rs" \
  -I"$H7RS_FW/Drivers/STM32H7RSxx_HAL_Driver/Inc" \
  -I"$H7RS_FW/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_h7rs.o"
echo "OK"

G4_FLAGS="-mcpu=cortex-m4 -mfloat-abi=soft -DSTM32G474xx \
  -I$HERE/g4 -I$PROJ/libs/stm32g4xx-hal-driver/Inc -I$PROJ/libs/cmsis-device-g4/Include"
# sed strips the objdump header naming the input file.
disasm() { "$OBJDUMP" -d -C "$1" | sed '1,3d'; }

echo "=== G4 equivalence: PROBE=0 must generate EXACTLY the no-probe code ==="
# The probe macro is stubbed out from the command line (the #ifndef in
# uart_probe.h exists for this build); the disassembly must be identical to
# the default build, proving the disabled Scope object costs zero code.
"$GCC" $COMMON_FLAGS $G4_FLAGS \
  -D'UART_ENGINE_PROBE_SCOPE(site)=((void)0)' \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4_noprobe.o"
disasm "$OUT/test_g4.o"         > "$OUT/test_g4.dis"
disasm "$OUT/test_g4_noprobe.o" > "$OUT/test_g4_noprobe.dis"
if ! diff -u "$OUT/test_g4_noprobe.dis" "$OUT/test_g4.dis"; then
  echo "FAIL: disabled probes changed the generated code"
  exit 1
fi
echo "OK (disassembly identical)"

echo "=== G4 probe-on: DWT backend compiles, all three scopes instantiated ==="
"$GCC" $COMMON_FLAGS $G4_FLAGS -DUART_ENGINE_PROBE=1 \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4_probe.o"
disasm "$OUT/test_g4_probe.o" > "$OUT/test_g4_probe.dis"
if cmp -s "$OUT/test_g4_probe.dis" "$OUT/test_g4.dis"; then
  echo "FAIL: PROBE=1 produced identical code - the scopes measured nothing"
  exit 1
fi
echo "OK"

echo "=== H7RS probe-on: CM7 DWT unlock path in uart_probe::init() ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m7 -mfloat-abi=soft -DSTM32H7S3xx \
  -DUART_ENGINE_PROBE=1 \
  -I"$HERE/h7rs" \
  -I"$H7RS_FW/Drivers/STM32H7RSxx_HAL_Driver/Inc" \
  -I"$H7RS_FW/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_h7rs_probe.o"
echo "OK"

echo "=== all targets compiled ==="
