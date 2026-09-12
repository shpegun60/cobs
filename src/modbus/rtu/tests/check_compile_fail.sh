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
OUT="$HERE/out"
CASES="$HERE/compile_fail"
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion"
mkdir -p "$OUT"
count=0

expect_failure() {
	source="$1"
	shift
	name="$(basename "$source" .cpp)"
	log="$OUT/compile_fail_$name.log"
	if "$CXX" -std=gnu++20 $WARN -I"$SRC" -I"$LIBS/delegate" \
		-fsyntax-only "$source" >"$log" 2>&1; then
		echo "FAIL  $name compiled, but this boundary must be rejected"
		exit 1
	fi
	for marker in "$@"; do
		if ! grep -F "$marker" "$log" >/dev/null; then
			echo "FAIL  $name failed for an unexpected reason; missing: $marker"
			cat "$log"
			exit 1
		fi
	done
	rm -f "$log"
	echo "  ok    $name rejected at the intended boundary"
	count=$((count + 1))
}

expect_failure "$CASES/storage_missing_tx.cpp" \
	"Endpoint storage must satisfy the wire::Storage contract"
expect_failure "$CASES/data_too_large.cpp" \
	"RTU data plus address, function and CRC must fit"
expect_failure "$CASES/data_size_overflow.cpp" \
	"RTU data plus address, function and CRC must fit"
expect_failure "$CASES/crc_data_too_large.cpp" \
	"RTU data plus address, function and CRC must fit"
expect_failure "$CASES/message_private_finalize.cpp" "finalize" "private"
expect_failure "$CASES/packet_private_adopt.cpp" "adopt" "private"
expect_failure "$CASES/append_struct.cpp" "append_be" "constraints not satisfied"
expect_failure "$CASES/crc_missing_calculate.cpp" \
	"RTU Format CRC must satisfy crc::Policy"
expect_failure "$CASES/crc_oversize.cpp" \
	"CRC wire_size must leave room for RTU address and function"
expect_failure "$CASES/framing_none_consume.cpp" "consume" "constraints not satisfied"
expect_failure "$CASES/framing_bad_policy.cpp" \
	"Endpoint framer must be framing::None or satisfy framing::Policy"

echo "$count Modbus compile-fail contracts rejected with expected diagnostics"
