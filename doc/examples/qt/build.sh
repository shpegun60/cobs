#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do
    [ "$ROOT" = / ] && { echo 'COBS.pro not found' >&2; exit 1; }
    ROOT="$(dirname "$ROOT")"
done
# MinGW kit with QtSerialPort + QtNetwork. Linux/macOS: set QMAKE and MAKE.
if [ -d /c/Qt/6.4.3/mingw_64 ]; then
    QT_KIT="${QT_KIT:-/c/Qt/6.4.3/mingw_64}"
    MINGW_BIN="${MINGW_BIN:-/c/Qt/Tools/mingw1120_64/bin}"
    export PATH="$QT_KIT/bin:$MINGW_BIN:$PATH"
    MAKE="${MAKE:-mingw32-make}"
else
    MAKE="${MAKE:-make}"
fi
QMAKE="${QMAKE:-qmake}"
for mode in cobs rtu tcp; do
    mkdir -p "$HERE/out/$mode"
    (
        cd "$HERE/out/$mode"
        if [ "$mode" = tcp ]; then project=tcp; else project=serial; fi
        "$QMAKE" "$HERE/$project.pro" "MODE=$mode" >qmake.log 2>&1 || { cat qmake.log; exit 1; }
        "$MAKE" -j4 >make.log 2>&1 || { cat make.log; exit 1; }
        binary="bin/qt_$mode"
        [ ! -f "$binary.exe" ] || binary="$binary.exe"
        "./$binary" --help
        "./$binary" --self-test
        if "./$binary" --invalid-option >invalid-option.log 2>&1; then
            echo "FAIL: qt_$mode accepted an invalid option" >&2; exit 1
        fi
        echo "qt_$mode: help, self-test and invalid-option rejection PASS"
    )
done
echo 'Qt cookbook: 3 event-loop programs passed; no COM port was opened.'
