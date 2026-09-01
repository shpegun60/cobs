#!/bin/sh
# Compile-only Cortex-M layout guard. Exact assertions live in test_layout.cpp;
# the nm output is retained as human-readable evidence without running target
# code. ARM_CXX may override the recorded CubeIDE toolchain.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
COBS="$(cd "$HERE/.." && pwd)"
PROJ="$(cd "$COBS/.." && pwd)"

DEFAULT_ARM_CXX="/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-g++.exe"
ARM_CXX="${ARM_CXX:-$DEFAULT_ARM_CXX}"
case "$ARM_CXX" in
	*g++.exe) DEFAULT_ARM_NM="${ARM_CXX%g++.exe}nm.exe" ;;
	*g++)     DEFAULT_ARM_NM="${ARM_CXX%g++}nm" ;;
	*)        DEFAULT_ARM_NM="arm-none-eabi-nm" ;;
esac
ARM_NM="${ARM_NM:-$DEFAULT_ARM_NM}"
OUT="$HERE/out"
OBJECT="$OUT/test_layout_arm.o"

if [ ! -x "$ARM_CXX" ]; then
	echo "ARM compiler not found: $ARM_CXX"
	echo "Set ARM_CXX to arm-none-eabi-g++ and optionally ARM_NM."
	exit 1
fi

mkdir -p "$OUT"

"$ARM_CXX" -c -std=gnu++20 -Os \
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion \
	-fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
	-mthumb -mcpu=cortex-m4 -mfloat-abi=soft \
	-I"$COBS" -I"$PROJ/libs/delegate" \
	"$HERE/test_layout.cpp" -o "$OBJECT"

echo "=== Cortex-M COBS layout (symbol sizes are hexadecimal bytes) ==="
"$ARM_NM" -S --size-sort "$OBJECT" | grep 'cobs_layout_'
echo "ARM layout assertions passed"
