#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Host codec microbenchmark. Timing is informational, never a CI pass/fail gate.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
COBS="$(cd "$HERE/../.." && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do
	[ "$ROOT" = "/" ] && { echo "repository root (COBS.pro) not found above $HERE" >&2; exit 1; }
	ROOT="$(dirname "$ROOT")"
done
SRC="$ROOT/src"
LIBS="$ROOT/libs"
CXX="${CXX:-g++}"
OUT="$HERE/../out/bench"
mkdir -p "$OUT"

"$CXX" -std=gnu++20 -O3 -DNDEBUG -march=native \
  -Wall -Wextra -Wpedantic -Wconversion \
  -I"$COBS" -I"$COBS/tests" -I"$LIBS/delegate" \
  "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/codec_bench.cpp" \
  -o "$OUT/codec_bench.exe"

"$CXX" -std=gnu++20 -O3 -DNDEBUG -march=native \
  -Wall -Wextra -Wpedantic -Wconversion \
  -I"$COBS" -I"$COBS/tests" -I"$LIBS/delegate" \
  "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/endpoint_bench.cpp" \
  -o "$OUT/endpoint_bench.exe"

"$OUT/codec_bench.exe"
"$OUT/endpoint_bench.exe"
