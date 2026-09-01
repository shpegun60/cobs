#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Negative API contracts. Every source below is intentionally invalid.
#
# Merely observing a non-zero compiler exit is too weak: a broken include path
# would make every case "pass". Each case must also contain the diagnostic
# markers that identify the boundary it was written to protect.

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
COBS="$(cd "$HERE/.." && pwd)"
PROJ="$(cd "$COBS/.." && pwd)"
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

	# shellcheck disable=SC2086
	if "$CXX" -std=gnu++20 $WARN -I"$COBS" -I"$PROJ/libs/delegate" \
	     -fsyntax-only "$source" >"$log" 2>&1; then
		echo "FAIL  $name compiled, but this boundary must be rejected"
		rm -f "$log"
		exit 1
	fi

	for marker in "$@"; do
		if ! grep -F "$marker" "$log" >/dev/null; then
			echo "FAIL  $name failed for an unexpected reason; missing: $marker"
			cat "$log"
			rm -f "$log"
			exit 1
		fi
	done

	echo "  ok    $name rejected at the intended boundary"
	rm -f "$log"
	count=$((count + 1))
}

expect_failure "$CASES/storage_missing_tx.cpp" \
	"Endpoint storage must satisfy the cobs::Storage contract"
expect_failure "$CASES/message_private_encode.cpp" \
	"encode" "private"
expect_failure "$CASES/packet_private_adopt.cpp" \
	"adopt" "private"
expect_failure "$CASES/append_struct.cpp" \
	"append_native" "constraints not satisfied"
expect_failure "$CASES/legacy_get_msg.cpp" \
	"get_msg" "no member named"
expect_failure "$CASES/legacy_set_sender.cpp" \
	"set_sender" "no member named"

echo "$count compile-fail contracts rejected with the expected diagnostics"
