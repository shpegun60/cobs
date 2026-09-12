#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do [ "$ROOT" != / ] || exit 1; ROOT="$(dirname "$ROOT")"; done
TOOLS="${ARM_TOOLS:-/c/ST/STM32CubeIDE_2.0.0/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.14.3.rel1.win32_1.0.100.202602081740/tools/bin}"
CXX="$TOOLS/arm-none-eabi-g++.exe"
OUT="$HERE/out/arm"
mkdir -p "$OUT"
for cpu in cortex-m0 cortex-m0plus cortex-m3 cortex-m4 cortex-m7 cortex-m23 cortex-m33 cortex-m55; do
    for opt in Os O2 O3; do
        for mode in little big strict; do
            case $mode in little) extra='-mlittle-endian';; big) extra='-mbig-endian';; strict) extra='-mlittle-endian -mno-unaligned-access';; esac
            stem="$OUT/$cpu-$opt-$mode"
            "$CXX" -std=c++20 -mcpu=$cpu -mthumb -mfloat-abi=soft -$opt $extra -fno-exceptions -fno-rtti \
                -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Werror -I"$ROOT/src" -isystem "$ROOT/libs/delegate" \
                -c "$HERE/arm_probe.cpp" -o "$stem.o"
            "$TOOLS/arm-none-eabi-nm.exe" -C -S "$stem.o" > "$stem.nm"
            "$TOOLS/arm-none-eabi-objdump.exe" -d -C "$stem.o" > "$stem.dis"
            if grep -Eq 'lookup_|crc::detail::Engine|HAL_|malloc|operator new|__aeabi_[df]' "$stem.nm"; then
                echo "default TCP gained CRC/transport/heap/FP dependency: $stem" >&2; exit 1
            fi
        done
    done
    echo "TCP $cpu: Os/O2/O3, little/big/strict, no CRC/table/heap/HAL/FP dependency"
done
for policy in 1 2; do
    stem="$OUT/crc-$policy"
    "$CXX" -std=c++20 -mcpu=cortex-m7 -mthumb -O2 -DTCP_PROBE_CRC=$policy -fno-exceptions -fno-rtti \
        -I"$ROOT/src" -isystem "$ROOT/libs/delegate" -c "$HERE/arm_probe.cpp" -o "$stem.o"
    "$TOOLS/arm-none-eabi-nm.exe" -C -S "$stem.o" > "$stem.nm"
done
if grep -q lookup_ "$OUT/crc-1.nm"; then echo 'Bitwise emitted a table' >&2; exit 1; fi
grep -Eq '00000200 .*lookup_' "$OUT/crc-2.nm" || { echo 'Table positive control missing 512-byte table' >&2; exit 1; }
echo 'TCP ARM: 72 default objects + Bitwise/Table positive controls passed'
