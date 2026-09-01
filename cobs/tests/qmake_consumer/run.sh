#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Out-of-tree qmake proof for cobs/cobs.pri. QMAKE and MAKE may override the
# Windows defaults; COBS_QMAKE_BUILD_DIR may select another build directory.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../../.." && pwd)"
BUILD="${COBS_QMAKE_BUILD_DIR:-$PROJ/build/cobs-consumer}"
QMAKE="${QMAKE:-qmake}"
MAKE="${MAKE:-mingw32-make}"

mkdir -p "$BUILD"
cd "$BUILD"

"$QMAKE" "$HERE/consumer.pro"
"$MAKE" -j

EXE="$BUILD/bin/cobs_pri_consumer.exe"
if [ ! -x "$EXE" ]; then
	EXE="$BUILD/bin/cobs_pri_consumer"
fi
"$EXE"
