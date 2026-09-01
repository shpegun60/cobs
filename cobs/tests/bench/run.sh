#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Host codec microbenchmark. Timing is informational, never a CI pass/fail gate.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
COBS="$(cd "$HERE/../.." && pwd)"
PROJ="$(cd "$COBS/.." && pwd)"
CXX="${CXX:-g++}"
OUT="$HERE/../out/bench"
mkdir -p "$OUT"

"$CXX" -std=gnu++20 -O3 -DNDEBUG -march=native \
  -Wall -Wextra -Wpedantic -Wconversion \
  -I"$COBS" -I"$COBS/tests" -I"$PROJ/libs/delegate" \
  "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/codec_bench.cpp" \
  -o "$OUT/codec_bench.exe"

"$CXX" -std=gnu++20 -O3 -DNDEBUG -march=native \
  -Wall -Wextra -Wpedantic -Wconversion \
  -I"$COBS" -I"$COBS/tests" -I"$PROJ/libs/delegate" \
  "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/endpoint_bench.cpp" \
  -o "$OUT/endpoint_bench.exe"

"$OUT/codec_bench.exe"
"$OUT/endpoint_bench.exe"
