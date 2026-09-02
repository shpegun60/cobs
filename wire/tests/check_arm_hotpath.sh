#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Compile-only proof for the endian scalar hot path on the STM32 toolchain.
# Both compiler byte orders must contain no runtime endian branch and no helper
# call. The native order is a direct load/store; the opposite order is
# REV/REV16 plus load/store.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
DEFAULT_ARM_CXX="/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-g++.exe"
ARM_CXX="${ARM_CXX:-$DEFAULT_ARM_CXX}"
case "$ARM_CXX" in
	*g++.exe) DEFAULT_OBJDUMP="${ARM_CXX%g++.exe}objdump.exe" ;;
	*g++)     DEFAULT_OBJDUMP="${ARM_CXX%g++}objdump" ;;
	*)        DEFAULT_OBJDUMP="arm-none-eabi-objdump" ;;
esac
ARM_OBJDUMP="${ARM_OBJDUMP:-$DEFAULT_OBJDUMP}"
OUT="$HERE/out"
LITTLE_OBJECT="$OUT/arm_hotpath_le.o"
LITTLE_ASSEMBLY="$OUT/arm_hotpath_le.asm"
BIG_OBJECT="$OUT/arm_hotpath_be.o"
BIG_ASSEMBLY="$OUT/arm_hotpath_be.asm"

if [ ! -x "$ARM_CXX" ]; then
	echo "ARM compiler not found: $ARM_CXX"
	exit 1
fi

mkdir -p "$OUT"
COMMON="-c -std=gnu++20 -Os \
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror \
	-fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
	-mthumb -mcpu=cortex-m7"

# shellcheck disable=SC2086
"$ARM_CXX" $COMMON -mlittle-endian -I"$PROJ" \
	"$HERE/arm_hotpath.cpp" -o "$LITTLE_OBJECT"
# shellcheck disable=SC2086
"$ARM_CXX" $COMMON -mbig-endian -I"$PROJ" \
	"$HERE/arm_hotpath.cpp" -o "$BIG_OBJECT"
"$ARM_OBJDUMP" -d "$LITTLE_OBJECT" > "$LITTLE_ASSEMBLY"
"$ARM_OBJDUMP" -d "$BIG_OBJECT" > "$BIG_ASSEMBLY"

ASSEMBLY="$LITTLE_ASSEMBLY"

function_body()
{
	awk "/<$1>:/,/^$/" "$ASSEMBLY"
}

require_instruction()
{
	name="$1"
	pattern="$2"
	description="$3"
	if ! function_body "$name" | grep -E "$pattern" >/dev/null; then
		echo "FAIL  $name: missing $description"
		function_body "$name"
		exit 1
	fi
}

forbid_instruction()
{
	name="$1"
	pattern="$2"
	description="$3"
	if function_body "$name" | grep -E "$pattern" >/dev/null; then
		echo "FAIL  $name: contains $description"
		function_body "$name"
		exit 1
	fi
}

# Little-endian target: LE is native and BE swaps.
require_instruction wire_store_be16 '[[:space:]]rev16[[:space:]]' 'REV16'
require_instruction wire_store_be16 '[[:space:]]strh[[:space:]]' 'halfword store'
require_instruction wire_store_be32 '[[:space:]]rev[[:space:]]' 'REV'
require_instruction wire_store_be32 '[[:space:]]str[[:space:]]' 'word store'
require_instruction wire_store_le32 '[[:space:]]str[[:space:]]' 'word store'
require_instruction wire_store_be64 '[[:space:]]rev[[:space:]]' 'two-word byte swap'
require_instruction wire_store_be_float '[[:space:]]rev[[:space:]]' 'floating bit-pattern byte swap'
require_instruction wire_load_be32 '[[:space:]]ldr[[:space:]]' 'word load'
require_instruction wire_load_be32 '[[:space:]]rev[[:space:]]' 'REV'
require_instruction wire_load_le32 '[[:space:]]ldr[[:space:]]' 'word load'
forbid_instruction wire_store_le32 '[[:space:]]rev(16)?[[:space:]]' 'a needless byte swap'
forbid_instruction wire_load_le32 '[[:space:]]rev(16)?[[:space:]]' 'a needless byte swap'

for name in wire_store_be16 wire_store_be32 wire_store_le32 wire_store_be64 \
	            wire_store_be_float \
	            wire_load_be32 wire_load_le32; do
	forbid_instruction "$name" '[[:space:]]blx?[[:space:]]' 'a helper call'
	forbid_instruction "$name" '[[:space:]](b(\.w|eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?|cbz|cbnz)[[:space:]]' 'runtime control flow'
done

# Big-endian compile: the exact same templates select the opposite static arm.
ASSEMBLY="$BIG_ASSEMBLY"
require_instruction wire_store_be16 '[[:space:]]strh[[:space:]]' 'halfword store'
require_instruction wire_store_be32 '[[:space:]]str[[:space:]]' 'word store'
require_instruction wire_store_le32 '[[:space:]]rev[[:space:]]' 'REV'
require_instruction wire_store_le32 '[[:space:]]str[[:space:]]' 'word store'
forbid_instruction wire_store_be64 '[[:space:]]rev(16)?[[:space:]]' 'a needless byte swap'
forbid_instruction wire_store_be_float '[[:space:]]rev(16)?[[:space:]]' 'a needless byte swap'
require_instruction wire_load_be32 '[[:space:]]ldr[[:space:]]' 'word load'
require_instruction wire_load_le32 '[[:space:]]ldr[[:space:]]' 'word load'
require_instruction wire_load_le32 '[[:space:]]rev[[:space:]]' 'REV'
forbid_instruction wire_store_be16 '[[:space:]]rev(16)?[[:space:]]' 'a needless byte swap'
forbid_instruction wire_store_be32 '[[:space:]]rev(16)?[[:space:]]' 'a needless byte swap'
forbid_instruction wire_load_be32 '[[:space:]]rev(16)?[[:space:]]' 'a needless byte swap'

for name in wire_store_be16 wire_store_be32 wire_store_le32 wire_store_be64 \
	            wire_store_be_float \
	            wire_load_be32 wire_load_le32; do
	forbid_instruction "$name" '[[:space:]]blx?[[:space:]]' 'a helper call'
	forbid_instruction "$name" '[[:space:]](b(\.w|eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?|cbz|cbnz)[[:space:]]' 'runtime control flow'
done

echo "ARM little/big endian hot paths are compile-time selected and call-free"
