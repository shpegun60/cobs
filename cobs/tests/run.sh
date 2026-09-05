#!/bin/sh
# Author: shpegun60
# SPDX-License-Identifier: MIT

# Host verification for the COBS layer. No HAL, no allocator fakes, no
# transport -- every suite here is an ordinary program.
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
PROJ="$(cd "$COBS/.." && pwd)"
CXX="${CXX:-g++}"
OUT="$HERE/out"
mkdir -p "$OUT"

echo "=== self-contained public headers ==="
CXX="$CXX" sh "$HERE/check_headers.sh"

echo "=== expected compile failures ==="
CXX="$CXX" sh "$HERE/check_compile_fail.sh"

WARN="-Wall -Wextra -Wpedantic -Wshadow -Wconversion"
CHECKED_STL="-D_GLIBCXX_ASSERTIONS"
SAN=""
if echo 'int main(){return 0;}' | "$CXX" -fsanitize=address,undefined -x c++ - \
     -o "$OUT/.sancheck" 2>/dev/null; then
  echo "=== sanitized build (address, undefined) ==="
  SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
else
  echo "=== plain build (no sanitizer runtime in this toolchain) ==="
fi
rm -f "$OUT/.sancheck" "$OUT/.sancheck.exe"

# One binary per layer, so a failure names the layer without a stack trace:
#   test_decoder         framing only
#   test_codec_exhaustive independent differential oracle over every short stream
#   shared storage and raw block pool are verified by wire/tests/run.sh
#   test_receiver        the internal RX vertical, end to end
#   test_packet          public packet lifetime/refcount via Endpoint only
#   test_encoder         canonical in-place encoding over its own payload
#   test_message         public message API plus coordinator-only TX transitions
#   test_endpoint        the assembled endpoint over a fake transport, both policies
#   test_layout          ABI snapshot for ownership-bearing public/current types
build() {
	name="$1"
	shift
	# shellcheck disable=SC2086
	"$CXX" -std=gnu++20 -O1 -g $WARN $CHECKED_STL $SAN -I"$COBS" -I"$HERE" -I"$PROJ/libs/delegate" "$@" -o "$OUT/$name.exe"
}

build_release() {
	name="$1"
	shift
	# Sanitizers intentionally stay out of this second build: its job is to
	# execute the actual optimized/NDEBUG codec, not repeat the O1 binary.
	# shellcheck disable=SC2086
	"$CXX" -std=gnu++20 -O3 -DNDEBUG $WARN $CHECKED_STL -I"$COBS" -I"$HERE" -I"$PROJ/libs/delegate" "$@" -o "$OUT/$name.exe"
}

build test_decoder         "$COBS/Decoder.cpp" "$HERE/test_decoder.cpp"
build test_geometry        "$HERE/test_geometry.cpp"
build test_crc             "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/test_crc.cpp"
build test_codec_exhaustive "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/test_codec_exhaustive.cpp"
build test_receiver        "$COBS/Decoder.cpp" "$HERE/test_receiver.cpp"
build test_packet          "$COBS/Decoder.cpp" "$HERE/test_packet.cpp"
build test_encoder         "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/test_encoder.cpp"
build test_message         "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/test_message.cpp"
build test_endpoint        "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/test_endpoint.cpp"
build test_layout          "$HERE/test_layout.cpp"

# Shared storage's independent NDEBUG guards live in wire/tests/run.sh.
build_release test_codec_exhaustive_o3 "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/test_codec_exhaustive.cpp"
build_release test_crc_o3 "$COBS/Decoder.cpp" "$COBS/Encoder.cpp" "$HERE/test_crc.cpp"

"$OUT/test_decoder.exe"
"$OUT/test_geometry.exe"
"$OUT/test_crc.exe"
"$OUT/test_codec_exhaustive.exe"
"$OUT/test_receiver.exe"
"$OUT/test_packet.exe"
"$OUT/test_encoder.exe"
"$OUT/test_message.exe"
"$OUT/test_endpoint.exe"
"$OUT/test_layout.exe"

echo "=== exhaustive codec oracle, optimized -O3/-DNDEBUG build ==="
"$OUT/test_codec_exhaustive_o3.exe"
"$OUT/test_crc_o3.exe"
