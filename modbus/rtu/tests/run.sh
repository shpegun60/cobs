#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../../.." && pwd)"
CXX="${CXX:-g++}"
OUT="$HERE/out"
mkdir -p "$OUT"

WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror"
CHECKED_STL="-D_GLIBCXX_ASSERTIONS"
SAN=""

echo "=== self-contained public headers ==="
CXX="$CXX" sh "$HERE/check_headers.sh"

echo "=== expected compile failures ==="
CXX="$CXX" sh "$HERE/check_compile_fail.sh"

if echo 'int main(){return 0;}' | "$CXX" -fsanitize=address,undefined -x c++ - \
	-o "$OUT/.sancheck" 2>/dev/null; then
	echo "=== sanitized build (address, undefined) ==="
	SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
else
	echo "=== plain build (no sanitizer runtime in this toolchain) ==="
fi
rm -f "$OUT/.sancheck" "$OUT/.sancheck.exe"

build() {
	name="$1"
	shift
	# shellcheck disable=SC2086
	"$CXX" -std=gnu++20 -O1 -g $WARN $CHECKED_STL $SAN \
		-I"$PROJ" -I"$PROJ/libs/delegate" "$@" -o "$OUT/$name.exe"
}

build_release() {
	name="$1"
	shift
	# shellcheck disable=SC2086
	"$CXX" -std=gnu++20 -O3 -DNDEBUG $WARN $CHECKED_STL \
		-I"$PROJ" -I"$PROJ/libs/delegate" "$@" -o "$OUT/$name.exe"
}

build test_pdu      "$HERE/test_pdu.cpp"
build test_crc      "$HERE/test_crc.cpp"
build test_crc_geometry "$HERE/test_crc_geometry.cpp"
build test_packet   "$HERE/test_packet.cpp"
build test_message  "$HERE/test_message.cpp"
build test_endpoint "$HERE/test_endpoint.cpp"
build test_fuzz     "$HERE/test_fuzz.cpp"
build test_layout   "$HERE/test_layout.cpp"
build_release test_fuzz_o3 "$HERE/test_fuzz.cpp"

"$OUT/test_pdu.exe"
"$OUT/test_crc.exe"
"$OUT/test_crc_geometry.exe"
"$OUT/test_packet.exe"
"$OUT/test_message.exe"
"$OUT/test_endpoint.exe"
"$OUT/test_fuzz.exe"
"$OUT/test_layout.exe"


echo "=== random/property suite under -O3/-DNDEBUG ==="
"$OUT/test_fuzz_o3.exe"

echo "=== all Modbus RTU host suites passed ==="
