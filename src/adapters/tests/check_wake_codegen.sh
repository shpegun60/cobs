#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do
	[ "$ROOT" = / ] && { echo 'repository root not found' >&2; exit 1; }
	ROOT="$(dirname "$ROOT")"
done
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
OUT="$HERE/out/wake-codegen"
mkdir -p "$OUT"
count=0
for cpu in cortex-m0 cortex-m4 cortex-m7; do
	for opt in Os O2 O3; do
		for bits in 16 32 64; do
			for hz in 100 1000 1024 10000; do
				tag="$cpu-$opt-$bits-$hz"
				"$TOOLS/arm-none-eabi-g++.exe" -c -std=c++20 -"$opt" -mthumb -mcpu="$cpu" \
					-Wall -Wextra -Wconversion -Werror -fno-exceptions -fno-rtti \
					-I"$ROOT/src" -I"$ROOT/libs/delegate" -I"$HERE/fake_freertos" \
					-DFAKE_FREERTOS_TICK_BITS="$bits" -DconfigTICK_RATE_HZ="$hz" \
					"$HERE/wake_hotpath.cpp" -o "$OUT/$tag.o"
				"$TOOLS/arm-none-eabi-objdump.exe" -d "$OUT/$tag.o" > "$OUT/$tag.dis"
				grep -q '<wake_default_ticks>:' "$OUT/$tag.dis"
				grep -q '<wake_variable_ticks>:' "$OUT/$tag.dis"
				# Ordinary 50-ms budgets remain compile-time constants.
				if awk '/<wake_default_ticks>:/,/^$/' "$OUT/$tag.dis" | grep -E '[[:space:]](bl|blx|b\.w)[[:space:]]' >/dev/null; then
					echo "FAIL $tag: default wait conversion calls a helper"; exit 1
				fi
				# At 1 kHz even dynamic durations need no arithmetic/copy helper.
				if [ "$hz" = 1000 ] && "$TOOLS/arm-none-eabi-nm.exe" -u "$OUT/$tag.o" | grep -E '__aeabi|memcpy|memmove' >/dev/null; then
					echo "FAIL $tag: 1-kHz variable conversion calls a helper"; exit 1
				fi
				count=$((count + 1))
			done
		done
	done
done
echo "PASS $count wake conversion objects; constant default and helper-free 1-kHz dynamic path"
