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

WARN="-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror"

build_run() {
  name="$1"
  shift
  exe="$OUT/$name.exe"
  log="$OUT/$name.log"
  echo "=== $name ==="
  "$CXX" -std=gnu++20 $WARN -D_GLIBCXX_ASSERTIONS \
    -I"$HERE" -I"$PROJ/uart" \
    -isystem "$PROJ/libs/spsc" -isystem "$PROJ/libs/spsc/src" \
    -isystem "$PROJ/libs/delegate" \
    "$@" "$HERE/fake_hal.cpp" "$HERE/test_uart.cpp" -o "$exe"
  if ! "$exe" >"$log"; then
    cat "$log"
    return 1
  fi
  tail -n 1 "$log"
}

build_run strict -O1 -g
build_run old-rxevent-fallback -O1 -g -DUART_ENGINE_HAS_RXEVENT_TYPE=0
build_run registered-callbacks -O1 -g -DUSE_HAL_UART_REGISTER_CALLBACKS=1
build_run external-callbacks -O1 -g -DUART_ENGINE_INTERNAL_CALLBACKS_ON=0
build_run optimized -O3 -DNDEBUG

expect_config_failure() {
  name="$1"
  diagnostic="$2"
  define="$3"
  log="$OUT/config-$name.log"
  echo "=== invalid config: $name ==="
  if "$CXX" -std=gnu++20 $WARN -I"$HERE" -I"$PROJ/uart" \
      -isystem "$PROJ/libs/spsc" -isystem "$PROJ/libs/spsc/src" \
      -isystem "$PROJ/libs/delegate" "$define" \
      -c "$HERE/config_compile.cpp" -o "$OUT/config-$name.o" 2>"$log"; then
    echo "FAIL: invalid configuration compiled: $name"
    return 1
  fi
  if ! grep -F "$diagnostic" "$log" >/dev/null; then
    cat "$log"
    echo "FAIL: expected diagnostic not found: $diagnostic"
    return 1
  fi
  echo "rejected with the intended diagnostic"
}

expect_config_failure max-instances-zero \
  "UART_ENGINE_MAX_INSTANCES must be at least 1" \
  -DUART_ENGINE_MAX_INSTANCES=0
expect_config_failure check-period-zero \
  "UART_ENGINE_CHECK_PERIOD_MS must fit a non-zero uint32_t interval" \
  -DUART_ENGINE_CHECK_PERIOD_MS=0
expect_config_failure check-period-overflow \
  "UART_ENGINE_CHECK_PERIOD_MS must fit a non-zero uint32_t interval" \
  -DUART_ENGINE_CHECK_PERIOD_MS=4294967296ULL
expect_config_failure fail-threshold-zero \
  "UART_ENGINE_FAIL_THRESHOLD must fit the uint8_t watchdog counters" \
  -DUART_ENGINE_FAIL_THRESHOLD=0
expect_config_failure fail-threshold-overflow \
  "UART_ENGINE_FAIL_THRESHOLD must fit the uint8_t watchdog counters" \
  -DUART_ENGINE_FAIL_THRESHOLD=256
expect_config_failure chunk-too-small \
  "ChunkSize must be >= 32" \
  -DUART_TEST_CHUNK_SIZE=28
expect_config_failure chunk-too-large \
  "HAL reception length is u16" \
  -DUART_TEST_CHUNK_SIZE=65536
expect_config_failure chunk-not-word-aligned \
  "ChunkSize must be word-aligned for DMA" \
  -DUART_TEST_CHUNK_SIZE=34
expect_config_failure chunk-count-too-small \
  "need at least 2 chunks to switch buffers" \
  -DUART_TEST_CHUNK_COUNT=1
expect_config_failure chunk-count-not-power-of-two \
  "ChunkCount must be a power of two (spsc)" \
  -DUART_TEST_CHUNK_COUNT=3

case "$("$CXX" -dumpmachine)" in
  *mingw*)
    echo "=== sanitizers skipped: MinGW target has no reliable ASan runtime ==="
    ;;
  *)
    ASAN_OPTIONS=halt_on_error=1:abort_on_error=1
    UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1
    export ASAN_OPTIONS UBSAN_OPTIONS
    build_run sanitizers -O1 -g -fsanitize=address,undefined \
      -fno-sanitize-recover=all -fno-omit-frame-pointer
    ;;
esac

echo "=== all host UART variants passed ==="
