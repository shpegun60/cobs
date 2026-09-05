#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Exact Cortex-M7 proof that Endpoint<> does not instantiate the lookup table,
# while Endpoint<Heap, crc::Table> owns one immutable 512-byte flash table.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../../.." && pwd)"
DEFAULT_ARM_CXX="/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin/arm-none-eabi-g++.exe"
ARM_CXX="${ARM_CXX:-$DEFAULT_ARM_CXX}"
case "$ARM_CXX" in
	*g++.exe)
		DEFAULT_ARM_NM="${ARM_CXX%g++.exe}nm.exe"
		DEFAULT_ARM_OBJDUMP="${ARM_CXX%g++.exe}objdump.exe"
		;;
	*g++)
		DEFAULT_ARM_NM="${ARM_CXX%g++}nm"
		DEFAULT_ARM_OBJDUMP="${ARM_CXX%g++}objdump"
		;;
	*)
		DEFAULT_ARM_NM="arm-none-eabi-nm"
		DEFAULT_ARM_OBJDUMP="arm-none-eabi-objdump"
		;;
esac
ARM_NM="${ARM_NM:-$DEFAULT_ARM_NM}"
ARM_OBJDUMP="${ARM_OBJDUMP:-$DEFAULT_ARM_OBJDUMP}"
OUT="$HERE/out/crc-codegen"
SOURCE="$HERE/crc_endpoint_codegen.cpp"
COMMON="-c -std=gnu++20 -DNDEBUG \
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror \
	-fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
	-mthumb -mcpu=cortex-m7 -mlittle-endian \
	-I$PROJ -I$PROJ/libs/delegate"

if [ ! -x "$ARM_CXX" ]; then
	echo "ARM compiler not found: $ARM_CXX"
	exit 1
fi

mkdir -p "$OUT"
count=0
for optimization in -Os -O2 -O3; do
	name="${optimization#-}"
	default_object="$OUT/endpoint_bitwise_$name.o"
	table_object="$OUT/endpoint_table_$name.o"
	default_symbols="$OUT/endpoint_bitwise_$name.nm"
	table_symbols="$OUT/endpoint_table_$name.nm"
	table_assembly="$OUT/endpoint_table_$name.asm"

	# shellcheck disable=SC2086
	"$ARM_CXX" $COMMON "$optimization" "$SOURCE" -o "$default_object"
	# shellcheck disable=SC2086
	"$ARM_CXX" $COMMON "$optimization" -DMODBUS_CRC_TABLE_PROBE=1 \
		"$SOURCE" -o "$table_object"
	"$ARM_NM" -S --size-sort "$default_object" > "$default_symbols"
	"$ARM_NM" -S --size-sort "$table_object" > "$table_symbols"
	"$ARM_OBJDUMP" -dr "$table_object" > "$table_assembly"

	if grep -F '_ZN6modbus3rtu3crc6detail5tableE' \
			"$default_symbols" >/dev/null; then
		echo "FAIL  Endpoint<> emitted the CRC lookup table at $optimization"
		exit 1
	fi
	if ! grep -E '00000200 [VvRr] _ZN6modbus3rtu3crc6detail5tableE$' \
			"$table_symbols" >/dev/null; then
		echo "FAIL  crc::Table did not emit exactly one 512-byte immutable table"
		cat "$table_symbols"
		exit 1
	fi
	if ! "$ARM_OBJDUMP" -t "$table_object" |
			grep -E '[[:space:]]O[[:space:]]+\.rodata\.[^[:space:]]+[[:space:]]+00000200[[:space:]]+_ZN6modbus3rtu3crc6detail5tableE$' \
			>/dev/null; then
		echo "FAIL  crc::Table lookup is not one 512-byte read-only object"
		exit 1
	fi
	if ! grep -E '[[:space:]]ldrh(\.w)?[[:space:]]' \
			"$table_assembly" >/dev/null; then
		echo "FAIL  crc::Table hot path has no halfword table load"
		exit 1
	fi
	count=$((count + 1))
done

echo "ARM Endpoint CRC codegen passed: $count optimization levels, no default table, one 512-byte Table"
