#!/bin/sh
# Every currently supported COBS header must compile as the first and only
# project include in a translation unit. This catches accidental dependence on
# include order across the staged namespace/file migration.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
COBS="$(cd "$HERE/.." && pwd)"
PROJ="$(cd "$COBS/.." && pwd)"
CXX="${CXX:-g++}"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion"

HEADERS="
 Cobs.h
 Codec.h
 Format.h
 CobsMsg.h
 CobsRx.h
 PacketRef.h
 Storage.h
"

count=0
for header in $HEADERS; do
	printf '#include "%s"\n' "$header" |
		"$CXX" -std=gnu++20 $WARN -I"$COBS" -I"$PROJ/libs/delegate" \
		-fsyntax-only -x c++ -
	count=$((count + 1))
done

echo "$count public headers compile independently"
