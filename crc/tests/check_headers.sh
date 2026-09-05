#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-g++}"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror"

printf '#include "Crc.h"\n' |
	"$CXX" -std=gnu++20 $WARN -I"$PROJ/crc" -fsyntax-only -x c++ -
printf '#include "crc/Crc.h"\n' |
	"$CXX" -std=gnu++20 $WARN -I"$PROJ" -fsyntax-only -x c++ -

echo "CRC public header compiles through local and repository include roots"
