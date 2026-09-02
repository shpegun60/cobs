#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../../.." && pwd)"
UART_HOST="$PROJ/uart/tests/host"
CXX="${CXX:-g++}"
OUT="$HERE/out"
mkdir -p "$OUT"

WARN="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror"
"$CXX" -std=gnu++20 -O1 -g $WARN -D_GLIBCXX_ASSERTIONS \
	-I"$UART_HOST" -I"$PROJ/uart" -I"$PROJ" \
	-isystem "$PROJ/libs/spsc" -isystem "$PROJ/libs/spsc/src" \
	-isystem "$PROJ/libs/delegate" \
	"$UART_HOST/fake_hal.cpp" "$HERE/test_uart_integration.cpp" \
	-o "$OUT/test_uart_integration.exe"

"$OUT/test_uart_integration.exe"
