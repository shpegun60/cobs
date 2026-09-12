#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Host verification for the shared wire layer: the scalar codec, the storage
# contract with its two built-in strategies, the raw block pool underneath
# them, and the API-parity guard that keeps the two protocol libraries on one
# vocabulary and one memory type.
#
# Sanitizers are used when the toolchain has them (WSL); MinGW ships no
# libasan and falls back to a plain -O1 build.
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
WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Werror"
CHECKED_STL="-D_GLIBCXX_ASSERTIONS"

mkdir -p "$OUT"

SAN=""
if echo 'int main(){return 0;}' | "$CXX" -fsanitize=address,undefined \
		-x c++ - -o "$OUT/.sancheck" 2>/dev/null; then
	echo "=== sanitized build (address, undefined) ==="
	SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
else
	echo "=== plain build (no sanitizer runtime in this toolchain) ==="
fi
rm -f "$OUT/.sancheck" "$OUT/.sancheck.exe"

build() {
	name="$1"
	shift
	# shellcheck disable=SC2086
	"$CXX" -std=gnu++20 -O1 -g $WARN $CHECKED_STL $SAN \
		-I"$SRC" -I"$LIBS/delegate" "$@" -o "$OUT/$name.exe"
}

build test_scalar     "$HERE/test_scalar.cpp"
build test_block_pool "$HERE/test_block_pool.cpp"
build test_storage    "$HERE/test_storage.cpp"
# Public vocabulary shared by the two protocol libraries, one memory type for
# both, plus the deliberate differences in receive framing and packet metadata.
build test_api_parity "$HERE/test_api_parity.cpp"
build test_protocol_storage "$HERE/test_protocol_storage.cpp" \
	"$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp"
build test_endpoint_parity "$HERE/test_endpoint_parity.cpp" \
	"$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp"
build test_payload_limits "$HERE/test_payload_limits.cpp" \
	"$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp"
build test_contracts "$HERE/test_contracts.cpp" \
	"$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp"
build test_length_domains "$HERE/test_length_domains.cpp"

# The release build is a DIFFERENT build, so it is tested as one: the pool's
# double-free and foreign-pointer rejection must survive -DNDEBUG, because a
# guarantee that evaporates in the shipped configuration is not a guarantee.
build test_block_pool_ndebug -DNDEBUG "$HERE/test_block_pool.cpp"
build test_storage_ndebug    -DNDEBUG "$HERE/test_storage.cpp"

"$OUT/test_scalar.exe"
"$OUT/test_block_pool.exe"
"$OUT/test_storage.exe"
"$OUT/test_api_parity.exe"
"$OUT/test_protocol_storage.exe"
"$OUT/test_endpoint_parity.exe"
"$OUT/test_payload_limits.exe"
"$OUT/test_contracts.exe"
"$OUT/test_length_domains.exe"

echo "=== the same pool guarantees, built with -DNDEBUG ==="
"$OUT/test_block_pool_ndebug.exe"
"$OUT/test_storage_ndebug.exe"

# A separate optimized build proves the exact release configuration too.
# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O3 -DNDEBUG -flto $WARN \
	-I"$SRC" "$HERE/test_scalar.cpp" -o "$OUT/test_scalar_o3_lto.exe"
"$OUT/test_scalar_o3_lto.exe"

# Public ownership/retry and framing contracts must survive the shipped build.
# shellcheck disable=SC2086
"$CXX" -std=gnu++20 -O3 -DNDEBUG -flto $WARN \
	-I"$SRC" -I"$LIBS/delegate" "$HERE/test_endpoint_parity.cpp" \
	"$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp" -o "$OUT/test_endpoint_parity_o3_lto.exe"
"$OUT/test_endpoint_parity_o3_lto.exe"
"$CXX" -std=gnu++20 -O3 -DNDEBUG -flto $WARN \
	-I"$SRC" -I"$LIBS/delegate" "$HERE/test_payload_limits.cpp" \
	"$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp" -o "$OUT/test_payload_limits_o3_lto.exe"
"$OUT/test_payload_limits_o3_lto.exe"
"$CXX" -std=gnu++20 -O3 -DNDEBUG -flto $WARN \
	-I"$SRC" -I"$LIBS/delegate" "$HERE/test_contracts.cpp" \
	"$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp" -o "$OUT/test_contracts_o3_lto.exe"
"$OUT/test_contracts_o3_lto.exe"
"$CXX" -std=gnu++20 -O3 -DNDEBUG -flto $WARN \
	-I"$SRC" -I"$LIBS/delegate" "$HERE/test_length_domains.cpp" \
	-o "$OUT/test_length_domains_o3_lto.exe"
"$OUT/test_length_domains_o3_lto.exe"

# Interpose malloc/free at the ELF linker boundary: exercise the real Heap,
# not a custom fake Storage. MinGW uses CRT import indirection instead.
case "$("$CXX" -dumpmachine)" in
	*mingw*) echo "Heap OOM allocator interposition requires ELF; run this guard under WSL" ;;
	*)
		build test_heap_oom -fno-builtin-malloc -fno-builtin-free -fno-builtin-calloc \
			"$HERE/test_heap_oom.cpp" "$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp" \
			-Wl,--wrap=malloc -Wl,--wrap=free
		"$OUT/test_heap_oom.exe"
		"$CXX" -std=gnu++20 -O3 -DNDEBUG -flto $WARN -fno-builtin-malloc -fno-builtin-free -fno-builtin-calloc \
			-I"$SRC" -I"$LIBS/delegate" "$HERE/test_heap_oom.cpp" \
			"$SRC/cobs/Encoder.cpp" "$SRC/cobs/Decoder.cpp" -Wl,--wrap=malloc -Wl,--wrap=free \
			-o "$OUT/test_heap_oom_o3_lto.exe"
		"$OUT/test_heap_oom_o3_lto.exe"
		;;
esac

echo "wire scalar, storage, block-pool and API-parity suites passed"
