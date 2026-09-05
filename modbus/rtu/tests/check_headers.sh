#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
RTU="$(cd "$HERE/.." && pwd)"
MODBUS="$(cd "$RTU/.." && pwd)"
PROJ="$(cd "$MODBUS/.." && pwd)"
CXX="${CXX:-g++}"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror"

ROOT_HEADERS="Types.h Pdu.h"
RTU_HEADERS="RtuLimits.h Format.h Stats.h Rtu.h"
count=0
for header in wire/Scalar.h wire/Read.h wire/Storage.h; do
	printf '#include "%s"\n' "$header" |
		"$CXX" -std=gnu++20 $WARN -I"$PROJ" -fsyntax-only -x c++ -
	count=$((count + 1))
done

for header in $ROOT_HEADERS; do
	printf '#include "%s"\n' "$header" |
		"$CXX" -std=gnu++20 $WARN -I"$MODBUS" -I"$PROJ/libs/delegate" \
		-fsyntax-only -x c++ -
	printf '#include "modbus/%s"\n' "$header" |
		"$CXX" -std=gnu++20 $WARN -I"$PROJ" -I"$PROJ/libs/delegate" \
		-fsyntax-only -x c++ -
	count=$((count + 1))
done

for header in $RTU_HEADERS; do
	printf '#include "%s"\n' "$header" |
		"$CXX" -std=gnu++20 $WARN -I"$RTU" -I"$PROJ/libs/delegate" \
		-fsyntax-only -x c++ -
	printf '#include "modbus/rtu/%s"\n' "$header" |
		"$CXX" -std=gnu++20 $WARN -I"$PROJ" -I"$PROJ/libs/delegate" \
		-fsyntax-only -x c++ -
	count=$((count + 1))
done

echo "$count Modbus/shared headers compile independently"
