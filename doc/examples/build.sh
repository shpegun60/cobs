#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT
# Runs the portable cookbook and the STM32/FreeRTOS examples on host fakes.
# Qt has its own real-event-loop runner: doc/examples/qt/build.sh.
# Hardware evidence is separate; this script never opens a COM port.
#
#   PATH="/c/Qt/Tools/mingw1310_64/bin:$PATH" sh doc/examples/build.sh
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do
	[ "$ROOT" = "/" ] && { echo "repository root (COBS.pro) not found above $HERE" >&2; exit 1; }
	ROOT="$(dirname "$ROOT")"
done
SRC="$ROOT/src"
LIBS="$ROOT/libs"
OUT="$HERE/out"
mkdir -p "$OUT"
CXX="${CXX:-g++}"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion"
EXTRA=""
if [ "${DOC_SANITIZE:-0}" = 1 ]; then
	case "$("$CXX" -dumpmachine)" in
		*linux*) EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie -no-pie" ;;
		*) echo "DOC_SANITIZE requires the Linux/WSL sanitizer runtime" >&2; exit 1 ;;
	esac
fi
INC="-I$SRC -I$SRC/cobs -I$SRC/uart -I$SRC/uart/tests/host -I$SRC/adapters/tests/fake_freertos      -I$SRC/modbus/rtu/tests -I$HERE      -isystem $LIBS/spsc -isystem $LIBS/spsc/src -isystem $LIBS/delegate"
status=0
run() {
	name="$1"; shift
	echo "=== $name ==="
	if "$CXX" -std=gnu++20 -O1 -g $WARN $EXTRA -D_GLIBCXX_ASSERTIONS $INC "$@" -o "$OUT/$name.exe" 2>"$OUT/$name.log"; then
		if grep -v "fake_hal.cpp" "$OUT/$name.log" | grep -qi "warning"; then
			grep -v "fake_hal.cpp" "$OUT/$name.log" | grep -i "warning"; status=1
		fi
		"$OUT/$name.exe" || status=1
	else
		cat "$OUT/$name.log"; status=1
	fi
}
run_hal() {
	name="$1"; source="$2"; shift 2
	run "$name" "-DDOC_EXAMPLE_SOURCE=\"$source\"" "$HERE/host_entry.cpp" "$@"
}
COBS_CORE="$SRC/cobs/Decoder.cpp $SRC/cobs/Encoder.cpp"
run_hal rtu_adapter rtu_adapter.cpp
run_hal cobs_direct cobs_direct.cpp $COBS_CORE
run_hal cobs_manual_wake cobs_direct.cpp -DDOC_WAKE=1 $COBS_CORE
run_hal cobs_adapter cobs_adapter.cpp $COBS_CORE
run_hal freertos_wake freertos_wake.cpp
run_hal cobs_freertos cobs_freertos.cpp $COBS_CORE
run rtu_direct    "$HERE/rtu_direct.cpp"
run any_transport "$HERE/any_transport.cpp" $COBS_CORE
run_hal uart_wake uart_wake.cpp
run_hal freertos_entry_cobs freertos_entry.cpp -DDOC_HOST=1 $COBS_CORE
run_hal freertos_entry_rtu freertos_entry.cpp -DDOC_HOST=1 -DDOC_RTU=1 $COBS_CORE
for name in protocols backpressure policies rtu_framing tcp_stream; do
	run "$name" "$HERE/$name.cpp" $COBS_CORE
done
if [ $status -eq 0 ]; then echo "=== all 16 host cookbook configurations passed (not hardware tests) ==="; else echo "=== cookbook FAILURES ==="; fi
exit $status
