#!/bin/sh
# Host verification for uart/Uart.h: builds the driver against the fake HAL in
# this directory and RUNS the suite. Unlike tests/port (compile-only), this
# executes the interleavings.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../../.." && pwd)"
CXX="${CXX:-g++}"
OUT="$HERE/out"
mkdir -p "$OUT"

"$CXX" -std=gnu++20 -O1 -g -Wall -Wextra \
  -I"$HERE" -I"$PROJ/uart" -I"$PROJ/libs/spsc/src" -I"$PROJ/libs/delegate" \
  "$HERE/fake_hal.cpp" "$HERE/test_uart.cpp" -o "$OUT/test_uart.exe"

"$OUT/test_uart.exe"
