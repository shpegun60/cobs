#!/bin/sh
# Compiles and runs the translation units behind doc/INTEGRATION.md against
# the real headers: the STM32 ones on the host fake HAL (uart/tests/host), the
# FreeRTOS one on the recording FreeRTOS fake. A snippet in the guide that no
# longer compiles fails here.
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
INC="-I$SRC -I$SRC/cobs -I$SRC/uart -I$SRC/uart/tests/host -I$SRC/adapters/tests/fake_freertos      -I$SRC/modbus/rtu/tests -I$HERE      -isystem $LIBS/spsc -isystem $LIBS/spsc/src -isystem $LIBS/delegate"
status=0
run() {
	name="$1"; shift
	echo "=== $name ==="
	if "$CXX" -std=gnu++20 -O1 -g $WARN -D_GLIBCXX_ASSERTIONS $INC "$@" -o "$OUT/$name.exe" 2>"$OUT/$name.log"; then
		if grep -v "fake_hal.cpp" "$OUT/$name.log" | grep -qi "warning"; then
			grep -v "fake_hal.cpp" "$OUT/$name.log" | grep -i "warning"; status=1
		fi
		"$OUT/$name.exe" || status=1
	else
		cat "$OUT/$name.log"; status=1
	fi
}
FAKE_HAL="$SRC/uart/tests/host/fake_hal.cpp"
COBS_CORE="$SRC/cobs/Decoder.cpp $SRC/cobs/Encoder.cpp"
run rtu_adapter   "$HERE/rtu_adapter.cpp"   "$FAKE_HAL"
run cobs_direct   "$HERE/cobs_direct.cpp"   "$FAKE_HAL" $COBS_CORE
run freertos_wake "$HERE/freertos_wake.cpp" "$FAKE_HAL"
run rtu_direct    "$HERE/rtu_direct.cpp"    "$FAKE_HAL"
run any_transport "$HERE/any_transport.cpp" $COBS_CORE
if [ $status -eq 0 ]; then echo "=== all INTEGRATION.md examples compiled and ran ==="; else echo "=== INTEGRATION.md example FAILURES ==="; fi
exit $status
