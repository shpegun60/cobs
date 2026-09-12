#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Host verification for src/adapters: the glue that knows both a transport and
# an endpoint, which neither of them may know. The STM32 adapter runs the real
# driver (src/uart/Uart.h) on the host fake HAL (src/uart/tests/host) through
# the matching UartAdapters into COBS and both RTU endpoint kinds; the FreeRTOS glue runs against the
# recording FreeRTOS fake in fake_freertos/. Sanitized under WSL:
#
#   wsl -e sh -c 'cd "$(wslpath "c:/Users/admin/Documents/my_workspace/Qt/COBS")" \
#                 && CXX=g++ sh src/adapters/tests/run.sh'
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

WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror"
INC="-I$SRC -I$SRC/uart -I$UART_HOST -I$HERE/fake_freertos -I$SRC/modbus/rtu/tests \
     -isystem $LIBS/spsc -isystem $LIBS/spsc/src -isystem $LIBS/delegate"

if echo 'int main(){return 0;}' | "$CXX" -fsanitize=address,undefined -x c++ - \
		-o "$OUT/san_probe.exe" >/dev/null 2>&1; then
	echo "=== sanitized build (address, undefined) ==="
	SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
else
	echo "=== plain build (no sanitizer runtime in this toolchain) ==="
	SAN=""
fi

echo "=== self-contained adapter headers ==="
# The STM32 adapters forward-declare/use the driver shape and include main.h
# for their HAL clock; they do not include the driver implementation.
# the FreeRTOS glue needs the two kernel headers, here the recording fake.
printf '#include "adapters/rtu/UartAdapter.h"\n' |
	"$CXX" -std=gnu++20 $WARN -I"$SRC" -I"$UART_HOST" -isystem "$LIBS/delegate" -fsyntax-only -x c++ -
printf '#include "adapters/cobs/UartAdapter.h"\n' |
	"$CXX" -std=gnu++20 $WARN -I"$SRC" -I"$UART_HOST" -isystem "$LIBS/delegate" -fsyntax-only -x c++ -
printf '#include "adapters/freertos/FreeRtosWake.h"\n' |
	"$CXX" -std=gnu++20 $WARN -I"$SRC" -I"$HERE/fake_freertos" -isystem "$LIBS/delegate" -fsyntax-only -x c++ -
echo "  ok    all adapter headers compile on their own"

echo "=== expected compile failures ==="
# The RTU adapter refuses anything that is not a modbus::rtu::Endpoint.
if "$CXX" -std=gnu++20 -fsyntax-only -I"$SRC" -I"$UART_HOST" -isystem "$LIBS/delegate" \
		"$HERE/compile_fail/adapter_needs_rtu_endpoint.cpp" >"$OUT/adapter_needs_rtu_endpoint.log" 2>&1; then
	echo "FAIL  adapter_needs_rtu_endpoint compiled"; exit 1
fi
grep -q "UartAdapter serves a modbus::rtu::Endpoint" "$OUT/adapter_needs_rtu_endpoint.log" ||
	{ echo "FAIL  adapter_needs_rtu_endpoint rejected for the wrong reason:"; cat "$OUT/adapter_needs_rtu_endpoint.log"; exit 1; }
echo "  ok    adapter_needs_rtu_endpoint rejected at the intended boundary"

if "$CXX" -std=gnu++20 -fsyntax-only -I"$SRC" -I"$UART_HOST" -isystem "$LIBS/delegate" \
		"$HERE/compile_fail/cobs_adapter_rejects_rtu.cpp" >"$OUT/cobs_adapter_rejects_rtu.log" 2>&1; then
	echo "FAIL  cobs_adapter_rejects_rtu compiled"; exit 1
fi
grep -q "UartAdapter serves a cobs::Endpoint" "$OUT/cobs_adapter_rejects_rtu.log" ||
	{ echo "FAIL  cobs_adapter_rejects_rtu rejected for the wrong reason:"; cat "$OUT/cobs_adapter_rejects_rtu.log"; exit 1; }
echo "  ok    framed RTU cannot silently lose its stale-frame supervision"

# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O1 -g $WARN -D_GLIBCXX_ASSERTIONS $SAN $INC \
	"$UART_HOST/fake_hal.cpp" "$HERE/test_uart_integration.cpp" -o "$OUT/test_uart_integration.exe"
# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O1 -g $WARN -D_GLIBCXX_ASSERTIONS $SAN $INC \
	"$UART_HOST/fake_hal.cpp" "$HERE/test_freertos_wake.cpp" -o "$OUT/test_freertos_wake.exe"
# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O1 -g $WARN -D_GLIBCXX_ASSERTIONS $SAN $INC \
	"$UART_HOST/fake_hal.cpp" "$HERE/test_uart_parity.cpp" \
	"$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp" -o "$OUT/test_uart_parity.exe"

"$OUT/test_uart_integration.exe"
"$OUT/test_freertos_wake.exe"
"$OUT/test_uart_parity.exe"

echo "=== adapter integration under -O3/-DNDEBUG ==="
# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O3 -DNDEBUG $WARN -D_GLIBCXX_ASSERTIONS $INC \
	"$UART_HOST/fake_hal.cpp" "$HERE/test_uart_integration.cpp" -o "$OUT/test_uart_integration_o3.exe"
"$OUT/test_uart_integration_o3.exe"
# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O3 -DNDEBUG $WARN -D_GLIBCXX_ASSERTIONS $INC \
	"$UART_HOST/fake_hal.cpp" "$HERE/test_uart_parity.cpp" \
	"$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp" -o "$OUT/test_uart_parity_o3.exe"
"$OUT/test_uart_parity_o3.exe"
echo "=== all adapter suites passed ==="
