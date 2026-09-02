#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../../.." && pwd)"
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
	if "$CXX" -std=gnu++20 $WARN -I"$PROJ" -I"$PROJ/libs/delegate" \
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
	"Endpoint storage must satisfy modbus::rtu::Storage"
expect_failure "$CASES/storage_wrong_geometry.cpp" \
	"Modbus RTU storage must provide the complete fixed RTU geometry"
expect_failure "$CASES/message_private_finalize.cpp" "finalize" "private"
expect_failure "$CASES/packet_private_adopt.cpp" "adopt" "private"
expect_failure "$CASES/append_struct.cpp" "append_be" "constraints not satisfied"

echo "$count Modbus compile-fail contracts rejected with expected diagnostics"
