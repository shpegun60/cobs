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
BUILD="${MODBUS_RTU_QMAKE_BUILD_DIR:-$ROOT/build/modbus-rtu-consumer}"
QMAKE="${QMAKE:-qmake}"
MAKE="${MAKE:-mingw32-make}"

mkdir -p "$BUILD"
cd "$BUILD"
"$QMAKE" "$HERE/consumer.pro"
"$MAKE" -j

EXE="$BUILD/bin/modbus_rtu_consumer.exe"
if [ ! -x "$EXE" ]; then
	EXE="$BUILD/bin/modbus_rtu_consumer"
fi
"$EXE"
