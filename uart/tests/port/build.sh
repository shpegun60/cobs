#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

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
NM="${GCC%g++.exe}nm.exe"
H7RS_FW="/c/Users/admin/STM32Cube/Repository/STM32Cube_FW_H7RS_V1.3.0"
CMSIS_CORE="$H7RS_FW/Drivers/CMSIS/Core/Include"

COMMON_FLAGS="-c -std=gnu++20 -Os -Wall -Wextra -Wpedantic -Wconversion \
  -Wsign-conversion -Wshadow -Werror -fno-exceptions -fno-rtti \
  -fstack-usage -ffunction-sections -fdata-sections -mthumb \
  -I$HERE -I$PROJ/uart -isystem $PROJ/libs/spsc \
  -isystem $PROJ/libs/spsc/src -isystem $PROJ/libs/delegate \
  -isystem $CMSIS_CORE"

OUT="$HERE/out"
mkdir -p "$OUT"
G4_FLAGS="-mcpu=cortex-m4 -mfloat-abi=soft -DSTM32G474xx \
  -I$HERE/g4 -isystem $PROJ/libs/stm32g4xx-hal-driver/Inc \
  -isystem $PROJ/libs/cmsis-device-g4/Include"

echo "=== F1 (STM32F103xE, Cortex-M3, legacy SR/DR) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m3 -mfloat-abi=soft -DSTM32F103xE \
  -I"$HERE/f1" \
  -isystem "$PROJ/libs/stm32f1xx-hal-driver/Inc" \
  -isystem "$PROJ/libs/cmsis-device-f1/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_f1.o"
echo "OK"

echo "=== G4 (STM32G474xx, Cortex-M4, new ISR/RDR, classic DMA) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m4 -mfloat-abi=soft -DSTM32G474xx \
  -I"$HERE/g4" \
  -isystem "$PROJ/libs/stm32g4xx-hal-driver/Inc" \
  -isystem "$PROJ/libs/cmsis-device-g4/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4.o"
echo "OK"

echo "=== G4 variant: UART_ENGINE_HAS_RXEVENT_TYPE=0 (old-HAL fallback branch) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m4 -mfloat-abi=soft -DSTM32G474xx \
  -DUART_ENGINE_HAS_RXEVENT_TYPE=0 \
  -I"$HERE/g4" \
  -isystem "$PROJ/libs/stm32g4xx-hal-driver/Inc" \
  -isystem "$PROJ/libs/cmsis-device-g4/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4_noevt.o"
echo "OK"

echo "=== G4 variant: USE_HAL_UART_REGISTER_CALLBACKS=1 (registered-callbacks init path) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m4 -mfloat-abi=soft -DSTM32G474xx \
  -DUSE_HAL_UART_REGISTER_CALLBACKS=1U \
  -I"$HERE/g4" \
  -isystem "$PROJ/libs/stm32g4xx-hal-driver/Inc" \
  -isystem "$PROJ/libs/cmsis-device-g4/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4_regcb.o"
echo "OK"

echo "=== G4 variant: UART_ENGINE_INTERNAL_CALLBACKS_ON=0 (external forwarding path) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m4 -mfloat-abi=soft -DSTM32G474xx \
  -DUART_ENGINE_INTERNAL_CALLBACKS_ON=0 \
  -I"$HERE/g4" \
  -isystem "$PROJ/libs/stm32g4xx-hal-driver/Inc" \
  -isystem "$PROJ/libs/cmsis-device-g4/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4_external_callbacks.o"
echo "OK"

echo "=== G4 optimized hot paths (-O3/-DNDEBUG) ==="
"$GCC" $COMMON_FLAGS -O3 -DNDEBUG $G4_FLAGS \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4_o3.o"
echo "OK"

echo "=== G4 static analyzer (all instantiated API paths) ==="
"$GCC" $COMMON_FLAGS -fanalyzer $G4_FLAGS \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_g4_analyzer.o"
echo "OK"

echo "=== H7RS (STM32H7S3xx, Cortex-M7, D-cache, GPDMA) ==="
"$GCC" $COMMON_FLAGS -mcpu=cortex-m7 -mfloat-abi=soft -DSTM32H7S3xx \
  -I"$HERE/h7rs" \
  -isystem "$H7RS_FW/Drivers/STM32H7RSxx_HAL_Driver/Inc" \
  -isystem "$H7RS_FW/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_h7rs.o"
echo "OK"

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
disasm "$OUT/test_g4_o3.o"      > "$OUT/test_g4_o3.dis"
if ! diff -u "$OUT/test_g4_noprobe.dis" "$OUT/test_g4.dis"; then
  echo "FAIL: disabled probes changed the generated code"
  exit 1
fi
echo "OK (disassembly identical)"

echo "=== G4 RX hot-path size/stack budget (pinned GCC 14.3, -Os) ==="
RX_SIZE=$("$NM" -S -C "$OUT/test_g4.o" \
  | grep -F 'Uart<256u, 4u>::init' \
  | grep -F 'lambda(void*, unsigned short)#1}::_FUN' \
  | awk '{print $2}' | head -n 1)
if [ "$RX_SIZE" != "00000054" ]; then
  echo "FAIL: RX callback thunk is $RX_SIZE bytes, expected 00000054"
  exit 1
fi
TX_SIZE=$("$NM" -S -C "$OUT/test_g4.o" \
  | grep -F 'Uart<256u, 4u>::init' \
  | grep -F 'lambda(void*)#1}::_FUN' \
  | awk '{print $2}' | head -n 1)
if [ "$TX_SIZE" != "00000050" ]; then
  echo "FAIL: TX callback thunk is $TX_SIZE bytes, expected 00000050"
  exit 1
fi
RX_STACK=$(grep -F 'Uart<256, 4>::init' "$OUT/test_g4.su" \
  | grep -F 'lambda(void*, uint16_t)' \
  | awk -F '\t' '{print $2}' | head -n 1)
if [ "$RX_STACK" != "8" ]; then
  echo "FAIL: RX callback stack is $RX_STACK bytes, expected 8"
  exit 1
fi
ARM_SIZE=$("$NM" -S -C "$OUT/test_g4.o" \
  | grep -F 'Uart<256u, 4u>::receiveArm()' | awk '{print $2}' | head -n 1)
PUBLISH_SIZE=$("$NM" -S -C "$OUT/test_g4.o" \
  | grep -F 'Uart<256u, 4u>::publishActive(unsigned short)' | awk '{print $2}' | head -n 1)
PROCEED_SIZE=$("$NM" -S -C "$OUT/test_g4.o" \
  | grep -F 'Uart<256u, 4u>::proceed(unsigned long)' | awk '{print $2}' | head -n 1)
if [ "$ARM_SIZE" != "0000006c" ] || \
   [ "$PUBLISH_SIZE" != "00000028" ] || \
   [ "$PROCEED_SIZE" != "00000024" ]; then
  echo "FAIL: hot symbols changed: receiveArm=$ARM_SIZE publish=$PUBLISH_SIZE proceed=$PROCEED_SIZE"
  exit 1
fi
echo "OK (RX 84 B/8 B stack, TX 80 B, arm 108 B, publish 40 B, idle proceed 36 B)"

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
  -isystem "$H7RS_FW/Drivers/STM32H7RSxx_HAL_Driver/Inc" \
  -isystem "$H7RS_FW/Drivers/CMSIS/Device/ST/STM32H7RSxx/Include" \
  "$HERE/test_instantiate.cpp" -o "$OUT/test_h7rs_probe.o"
echo "OK"

echo "=== all targets compiled ==="
