#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do
	[ "$ROOT" = "/" ] && { echo "repository root (COBS.pro) not found above $HERE" >&2; exit 1; }
	ROOT="$(dirname "$ROOT")"
done
SRC="$ROOT/src"
LIBS="$ROOT/libs"
UART_HOST="$SRC/uart/tests/host"
CXX="${CXX:-g++}"
OUT="$HERE/out"
mkdir -p "$OUT"

WARN="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror"
"$CXX" -std=gnu++20 -O1 -g $WARN -D_GLIBCXX_ASSERTIONS \
	-I"$UART_HOST" -I"$SRC/uart" -I"$SRC" \
	-isystem "$LIBS/spsc" -isystem "$LIBS/spsc/src" \
	-isystem "$LIBS/delegate" \
	"$UART_HOST/fake_hal.cpp" "$HERE/test_uart_integration.cpp" \
	-o "$OUT/test_uart_integration.exe"

"$OUT/test_uart_integration.exe"
