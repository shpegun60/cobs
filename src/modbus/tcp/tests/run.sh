#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do [ "$ROOT" != / ] || exit 1; ROOT="$(dirname "$ROOT")"; done
CXX="${CXX:-g++}"
OUT="$HERE/out"
mkdir -p "$OUT"
CXX="$CXX" sh "$HERE/check_contracts.sh"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror"
SAN=""
if echo 'int main(){}' | "$CXX" -x c++ - -fsanitize=address,undefined -o "$OUT/sanprobe" 2>/dev/null; then
    SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
    echo 'TCP: ASan + UBSan enabled'
else
    echo 'TCP: toolchain has no sanitizer runtime (use WSL for sanitized tests)'
fi
for name in core advanced data_limits; do
    "$CXX" -std=c++20 -O1 -g $WARN $SAN -D_GLIBCXX_ASSERTIONS -I"$ROOT/src" -I"$ROOT/libs/delegate" \
        "$HERE/test_$name.cpp" -o "$OUT/test_$name.exe"
    "$OUT/test_$name.exe"
    "$CXX" -std=c++20 -O3 -flto -DNDEBUG $WARN -D_GLIBCXX_ASSERTIONS -I"$ROOT/src" -I"$ROOT/libs/delegate" \
        "$HERE/test_$name.cpp" -o "$OUT/test_${name}_o3.exe"
    "$OUT/test_${name}_o3.exe"
done
echo 'TCP sanitized/plain and O3/LTO suites passed'
