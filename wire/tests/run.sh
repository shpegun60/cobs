#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-g++}"
OUT="$HERE/out"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror"

mkdir -p "$OUT"

SAN=""
if echo 'int main(){return 0;}' | "$CXX" -fsanitize=address,undefined \
		-x c++ - -o "$OUT/.sancheck" 2>/dev/null; then
	SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
fi
rm -f "$OUT/.sancheck" "$OUT/.sancheck.exe"

# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O1 -g $WARN $SAN -D_GLIBCXX_ASSERTIONS \
	-I"$PROJ" "$HERE/test_scalar.cpp" -o "$OUT/test_scalar.exe"
"$OUT/test_scalar.exe"

# A separate optimized build proves the exact release configuration too.
# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O3 -DNDEBUG -flto $WARN \
	-I"$PROJ" "$HERE/test_scalar.cpp" -o "$OUT/test_scalar_o3_lto.exe"
"$OUT/test_scalar_o3_lto.exe"

echo "wire scalar host and O3/LTO suites passed"
