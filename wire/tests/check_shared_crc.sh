#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-g++}"
NM="${NM:-nm}"
FLAGS="${CXXFLAGS:-}"
OUT="$HERE/out/shared-crc"
mkdir -p "$OUT"
for optimization in Os O2 O3; do
    for policy in 0 1 2; do
        for protocol in 0 1; do
            "$CXX" $FLAGS -std=c++20 -"$optimization" -DNDEBUG -fno-exceptions -fno-rtti \
                -DTEST_CRC="$policy" -DTEST_COBS="$protocol" \
                -I"$PROJ" -I"$PROJ/libs/delegate" -c "$HERE/integrity_codegen.cpp" \
                -o "$OUT/$optimization-$policy-$protocol.o"
        done
        "$CXX" $FLAGS -nostdlib -r "$OUT/$optimization-$policy-0.o" "$OUT/$optimization-$policy-1.o" \
            -o "$OUT/$optimization-$policy-linked.o"
        "$NM" -S -C "$OUT/$optimization-$policy-linked.o" > "$OUT/symbols.txt"
        count=$(grep -c '::lookup_' "$OUT/symbols.txt" || true)
        if [ "$policy" = 2 ]; then
            [ "$count" = 1 ]
            grep -E '0*200 [ruV] .*::lookup_' "$OUT/symbols.txt" >/dev/null
        else
            [ "$count" = 0 ]
        fi
    done
done
echo 'Shared COBS + RTU: 3 optimization levels; NoCrc/Bitwise emit zero tables; two Table translation units link one 512-byte lookup'
