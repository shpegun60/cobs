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
CXX="${CXX:-g++}"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror"

printf '#include "Crc.h"\n' |
	"$CXX" -std=gnu++20 $WARN -I"$SRC/crc" -fsyntax-only -x c++ -
printf '#include "crc/Crc.h"\n' |
	"$CXX" -std=gnu++20 $WARN -I"$SRC" -fsyntax-only -x c++ -

echo "CRC public header compiles through local and repository include roots"
