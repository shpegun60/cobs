#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Every currently supported COBS header must compile as the first and only
# project include in a translation unit. This catches accidental dependence on
# include order across the staged namespace/file migration.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
COBS="$(cd "$HERE/.." && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do
	[ "$ROOT" = "/" ] && { echo "repository root (COBS.pro) not found above $HERE" >&2; exit 1; }
	ROOT="$(dirname "$ROOT")"
done
SRC="$ROOT/src"
LIBS="$ROOT/libs"
CXX="${CXX:-g++}"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion"

HEADERS="
 Cobs.h
 Codec.h
 Format.h
 Read.h
 Stats.h
"

count=0
for header in wire/Scalar.h wire/Read.h wire/Storage.h; do
	printf '#include "%s"\n' "$header" |
		"$CXX" -std=gnu++20 $WARN -I"$SRC" -fsyntax-only -x c++ -
	count=$((count + 1))
done
for header in $HEADERS; do
	printf '#include "%s"\n' "$header" |
		"$CXX" -std=gnu++20 $WARN -I"$COBS" -I"$LIBS/delegate" \
		-fsyntax-only -x c++ -
	printf '#include "cobs/%s"\n' "$header" |
		"$CXX" -std=gnu++20 $WARN -I"$SRC" -I"$LIBS/delegate" \
		-fsyntax-only -x c++ -
	count=$((count + 1))
done

echo "$count COBS/shared headers compile independently"
