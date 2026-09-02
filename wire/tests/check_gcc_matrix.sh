#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Strict GCC consumer/LTO proof shared by COBS and Modbus. Run it once per
# installed compiler with a distinct MATRIX_TAG; all outputs remain ignored.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-g++}"
TAG="${MATRIX_TAG:-default}"
OUT="$HERE/out/gcc-matrix-$TAG"

case "$TAG" in
	*[!A-Za-z0-9_.-]*|'')
		echo "MATRIX_TAG must contain only letters, digits, dot, underscore or dash"
		exit 1
		;;
esac

mkdir -p "$OUT"

COMMON="-std=gnu++20 -O3 -DNDEBUG -flto \
	-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror \
	-Wcast-align=strict -Warray-bounds=2 -Wstringop-overflow=4 \
	-Wnull-dereference -Wstrict-aliasing=3 -Wundef -Wformat=2 \
	-D_GLIBCXX_ASSERTIONS -I$PROJ -I$PROJ/cobs -I$PROJ/cobs/tests \
	-I$PROJ/libs/delegate -I$PROJ/libs/spsc"

# shellcheck disable=SC2086
"$CXX" $COMMON \
	"$PROJ/cobs/Decoder.cpp" \
	"$PROJ/cobs/Encoder.cpp" \
	"$PROJ/cobs/tests/qmake_consumer/main.cpp" \
	-o "$OUT/cobs_consumer.exe"
"$OUT/cobs_consumer.exe"

# shellcheck disable=SC2086
"$CXX" $COMMON \
	"$PROJ/modbus/rtu/tests/qmake_consumer/main.cpp" \
	-o "$OUT/modbus_consumer.exe"
"$OUT/modbus_consumer.exe"

# Compile-time contract for the common public ownership/reader vocabulary and
# the explicitly protocol-specific receive/metadata surface.
# shellcheck disable=SC2086
"$CXX" $COMMON "$HERE/test_api_parity.cpp" \
	-o "$OUT/api_parity.exe"
"$OUT/api_parity.exe"

# These options intentionally perturb implementation-defined ABI choices.
# Wire bytes must remain unchanged because protocol order is explicit.
# shellcheck disable=SC2086
"$CXX" $COMMON -fshort-enums -funsigned-char \
	"$HERE/test_scalar.cpp" -o "$OUT/scalar_abi_flags.exe"
"$OUT/scalar_abi_flags.exe"

# Compile-only wrappers keep every scalar/protocol hot path available for
# warning and aliasing analysis without depending on a benchmark main().
# shellcheck disable=SC2086
"$CXX" $COMMON -c "$HERE/protocol_hotpath.cpp" \
	-o "$OUT/protocol_hotpath.o"

echo "GCC strict O3/LTO matrix passed: $TAG"
