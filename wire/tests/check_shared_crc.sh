#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Links a COBS translation unit and a Modbus RTU translation unit that select
# the same CRC policy, then proves what the pair emits: no lookup bytes for
# NoCrc or Bitwise, and exactly ONE 512-byte read-only table when both pick
# crc::Crc16Table. Runs at -Os, -O2 and -O3.
#
# Every check reports what it saw before exiting, so a failure is a diagnosis
# rather than a silent non-zero status.
#
# Object format: the size column of `nm -S` is required. ELF toolchains
# (Linux GCC, arm-none-eabi) provide it; MinGW's COFF objects do not, so on a
# Windows host run this under WSL or with the ARM toolchain:
#
#   CXX=arm-none-eabi-g++ NM=arm-none-eabi-nm \
#   CXXFLAGS="-mthumb -mcpu=cortex-m7 -mfloat-abi=soft" sh wire/tests/check_shared_crc.sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
CXX="${CXX:-g++}"
NM="${NM:-nm}"
FLAGS="${CXXFLAGS:-}"
OUT="$HERE/out/shared-crc"
mkdir -p "$OUT"

fail() {
	echo "FAIL  $*" >&2
	exit 1
}

# shellcheck disable=SC2086
if ! "$CXX" $FLAGS -dM -E -x c++ /dev/null 2>/dev/null | grep -q '__ELF__'; then
	fail "$CXX does not target ELF objects; nm -S carries no symbol sizes on COFF." \
	     "Run this guard under WSL or with the arm-none-eabi toolchain (see the header)."
fi

for optimization in Os O2 O3; do
	for policy in 0 1 2; do
		for protocol in 0 1; do
			# shellcheck disable=SC2086
			"$CXX" $FLAGS -std=c++20 -"$optimization" -DNDEBUG -fno-exceptions -fno-rtti \
				-DTEST_CRC="$policy" -DTEST_COBS="$protocol" \
				-I"$PROJ" -I"$PROJ/libs/delegate" -c "$HERE/integrity_codegen.cpp" \
				-o "$OUT/$optimization-$policy-$protocol.o" ||
				fail "compile $optimization policy=$policy protocol=$protocol"
		done
		# shellcheck disable=SC2086
		"$CXX" $FLAGS -nostdlib -r "$OUT/$optimization-$policy-0.o" "$OUT/$optimization-$policy-1.o" \
			-o "$OUT/$optimization-$policy-linked.o" || fail "link $optimization policy=$policy"
		"$NM" -S -C "$OUT/$optimization-$policy-linked.o" > "$OUT/symbols.txt"
		count=$(grep -c '::lookup_' "$OUT/symbols.txt" || true)
		if [ "$policy" = 2 ]; then
			[ "$count" = 1 ] ||
				fail "$optimization Table: expected exactly one lookup symbol, found $count:" \
				     "$(grep '::lookup_' "$OUT/symbols.txt" || echo none)"
			grep -E '0*200 [ruV] .*::lookup_' "$OUT/symbols.txt" >/dev/null ||
				fail "$optimization Table: the lookup is not one 512-byte read-only object:" \
				     "$(grep '::lookup_' "$OUT/symbols.txt")"
		else
			[ "$count" = 0 ] ||
				fail "$optimization policy=$policy emitted a lookup that no table policy asked for:" \
				     "$(grep '::lookup_' "$OUT/symbols.txt")"
		fi
	done
done
echo 'Shared COBS + RTU: 3 optimization levels; NoCrc/Bitwise emit zero tables; two Table translation units link one 512-byte lookup'
