#!/bin/sh
# Host verification for the COBS layer. No HAL, no allocator fakes, no
# transport — every suite here is an ordinary program.
#
# Sanitizers are used when the toolchain has them: a bounds error in a decoder
# must fail loudly here, not become a discussion about why a test only fails on
# Thursdays. MinGW ships no libasan, so the build falls back to plain -O1
# there. On this machine the sanitized run is one command away:
#
#   wsl -e sh -c 'cd "$(wslpath "c:/Users/admin/Documents/my_workspace/Qt/COBS")" \
#                 && CXX=g++ sh cobs/tests/run.sh'
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
COBS="$(cd "$HERE/.." && pwd)"
CXX="${CXX:-g++}"
OUT="$HERE/out"
mkdir -p "$OUT"

WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion"
SAN=""
if echo 'int main(){return 0;}' | "$CXX" -fsanitize=address,undefined -x c++ - \
     -o "$OUT/.sancheck" 2>/dev/null; then
  echo "=== sanitized build (address, undefined) ==="
  SAN="-fsanitize=address,undefined -fno-omit-frame-pointer"
else
  echo "=== plain build (no sanitizer runtime in this toolchain) ==="
fi
rm -f "$OUT/.sancheck" "$OUT/.sancheck.exe"

# One binary per layer, so a failure names the layer without a stack trace:
#   test_decoder         framing only
#   test_block_pool      detail::StaticBlockPool, the raw memory primitive
#   test_allocators      the policy contract, run against BOTH policies
#   test_cobs_rx         the assembled RX vertical, end to end
#   test_encoder         canonical in-place encoding over its own payload
#   test_cobs_msg        TX block ownership and geometry, no transport
build() {
	name="$1"
	shift
	# shellcheck disable=SC2086
	"$CXX" -std=gnu++20 -O1 -g $WARN $SAN -I"$COBS" -I"$HERE" "$@" -o "$OUT/$name.exe"
}

build test_decoder         "$COBS/CobsDecoder.cpp" "$HERE/test_decoder.cpp"
build test_block_pool      "$HERE/test_block_pool.cpp"
build test_allocators      "$HERE/test_allocators.cpp"
build test_cobs_rx         "$COBS/CobsDecoder.cpp" "$HERE/test_cobs_rx.cpp"
build test_encoder         "$COBS/CobsDecoder.cpp" "$COBS/CobsEncoder.cpp" "$HERE/test_encoder.cpp"
build test_cobs_msg        "$COBS/CobsEncoder.cpp" "$HERE/test_cobs_msg.cpp"

"$OUT/test_decoder.exe"
"$OUT/test_block_pool.exe"
"$OUT/test_allocators.exe"
"$OUT/test_cobs_rx.exe"
"$OUT/test_encoder.exe"
"$OUT/test_cobs_msg.exe"
