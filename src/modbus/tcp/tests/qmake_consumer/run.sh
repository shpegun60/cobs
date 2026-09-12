#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do [ "$ROOT" != / ] || exit 1; ROOT="$(dirname "$ROOT")"; done
BUILD="${MODBUS_TCP_QMAKE_BUILD_DIR:-$ROOT/build/modbus-tcp-consumer}"
mkdir -p "$BUILD"
cd "$BUILD"
"${QMAKE:-qmake}" "$HERE/consumer.pro"
"${MAKE:-mingw32-make}" -j
"$BUILD/bin/modbus_tcp_consumer.exe"
