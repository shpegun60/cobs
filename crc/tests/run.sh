#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-g++}"
OUT="$HERE/out"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror"
CHECKED_STL="-D_GLIBCXX_ASSERTIONS"
SAN=""
SAN_STATUS="plain -O1 (no sanitizer runtime in this toolchain; MinGW ships none, WSL g++ does)"
mkdir -p "$OUT"

CXX="$CXX" sh "$HERE/check_headers.sh"

# Sanitizers only when the toolchain actually has the runtime. The final line
# reports which build ran, so a MinGW run cannot claim coverage it did not have.
if echo 'int main(){return 0;}' | "$CXX" -fsanitize=address,undefined -x c++ - \
	-o "$OUT/.sancheck" 2>/dev/null; then
	SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
	SAN_STATUS="sanitized -O1 (address, undefined)"
fi
rm -f "$OUT/.sancheck" "$OUT/.sancheck.exe"
echo "=== $SAN_STATUS ==="

# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O1 -g $WARN $CHECKED_STL $SAN \
	-I"$PROJ" "$HERE/test_crc.cpp" -o "$OUT/test_crc.exe"
"$OUT/test_crc.exe"

# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O3 -DNDEBUG $WARN $CHECKED_STL \
	-I"$PROJ" "$HERE/test_crc.cpp" -o "$OUT/test_crc_o3.exe"
"$OUT/test_crc_o3.exe"

echo "CRC host suites passed: $SAN_STATUS, then -O3 -DNDEBUG"
