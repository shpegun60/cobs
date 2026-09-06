#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Host verification for adapters/qt: the SerialAdapter and the QModbus-shaped
# RtuClient on a QIODevice stand-in for QSerialPort (both protocols, a real
# event loop, no COM port), plus the compile-fail contract. Needs a Qt kit with
# QtSerialPort; on this machine that is Qt 6.4.3 MinGW 64-bit with its own
# GCC 11.2 (the 6.10.1 kit has no SerialPort module):
#
#   sh src/adapters/qt/tests/run.sh
#   QT_KIT=/c/Qt/6.4.3/mingw_64 MINGW_BIN=/c/Qt/Tools/mingw1120_64/bin sh src/adapters/qt/tests/run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do
	[ "$ROOT" = "/" ] && { echo "repository root (COBS.pro) not found above $HERE" >&2; exit 1; }
	ROOT="$(dirname "$ROOT")"
done
SRC="$ROOT/src"
LIBS="$ROOT/libs"
QT_KIT="${QT_KIT:-/c/Qt/6.4.3/mingw_64}"
MINGW_BIN="${MINGW_BIN:-/c/Qt/Tools/mingw1120_64/bin}"
export PATH="$QT_KIT/bin:$MINGW_BIN:$PATH"
OUT="$HERE/out"
mkdir -p "$OUT/host"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Werror"
QT_INC="-isystem $QT_KIT/include -isystem $QT_KIT/include/QtCore -isystem $QT_KIT/include/QtSerialPort"

echo "=== Qt kit: $(qmake -query QT_VERSION) at $QT_KIT, $(g++ --version | head -n 1) ==="

echo "=== self-contained public headers ==="
for header in adapters/qt/SerialAdapter.h adapters/qt/RtuClient.h; do
	printf '#include "%s"\n' "$header" |
		g++ -std=gnu++20 $WARN -I"$SRC" -isystem "$LIBS/delegate" $QT_INC -fsyntax-only -x c++ -
	echo "  ok    $header compiles on its own"
done

echo "=== expected compile failures ==="
if g++ -std=gnu++20 -fsyntax-only -I"$SRC" -isystem "$LIBS/delegate" $QT_INC \
		"$HERE/compile_fail/burst_rtu_over_serial.cpp" >"$OUT/burst_rtu_over_serial.log" 2>&1; then
	echo "FAIL  burst_rtu_over_serial compiled"; exit 1
fi
grep -q "an RTU endpoint over QSerialPort needs a framing policy" "$OUT/burst_rtu_over_serial.log" ||
	{ echo "FAIL  burst_rtu_over_serial rejected for the wrong reason:"; cat "$OUT/burst_rtu_over_serial.log"; exit 1; }
echo "  ok    burst_rtu_over_serial rejected at the intended boundary"

echo "=== serial_adapter_test (qmake, $QT_KIT) ==="
(
	cd "$OUT/host"
	qmake "$HERE/serial_adapter_test/serial_adapter_test.pro" >qmake.log 2>&1
	mingw32-make -j >make.log 2>&1 || { cat make.log; exit 1; }
	if grep -i "warning" make.log | grep -v "/Qt/"; then
		echo "FAIL  the test build is not warning-free"; exit 1
	fi
)
"$OUT/host/bin/serial_adapter_test.exe"
echo "=== all Qt adapter suites passed ==="
