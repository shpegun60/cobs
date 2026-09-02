#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../../.." && pwd)"
DEFAULT_ARM_CXX="/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-g++.exe"
ARM_CXX="${ARM_CXX:-$DEFAULT_ARM_CXX}"
case "$ARM_CXX" in
	*g++.exe) DEFAULT_ARM_NM="${ARM_CXX%g++.exe}nm.exe" ;;
	*g++) DEFAULT_ARM_NM="${ARM_CXX%g++}nm" ;;
	*) DEFAULT_ARM_NM="arm-none-eabi-nm" ;;
esac
ARM_NM="${ARM_NM:-$DEFAULT_ARM_NM}"
OUT="$HERE/out"
OBJECT="$OUT/test_layout_arm.o"

if [ ! -x "$ARM_CXX" ]; then
	echo "ARM compiler not found: $ARM_CXX"
	exit 1
fi

mkdir -p "$OUT"
"$ARM_CXX" -c -std=gnu++20 -Os \
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror \
	-fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
	-mthumb -mcpu=cortex-m4 -mfloat-abi=soft \
	-I"$PROJ" -I"$PROJ/libs/delegate" \
	"$HERE/test_layout.cpp" -o "$OBJECT"

echo "=== Cortex-M Modbus RTU layout (hexadecimal bytes) ==="
"$ARM_NM" -S --size-sort "$OBJECT" | grep 'modbus_layout_'
echo "ARM Modbus RTU layout assertions passed"
