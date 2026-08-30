#!/bin/sh
# Host verification for cobs/CobsDecoder. No HAL, no allocator, no transport —
# the decoder is a plain non-template class, so this is an ordinary program.
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
SRC="$COBS/CobsDecoder.cpp $HERE/test_decoder.cpp"

if echo 'int main(){return 0;}' | "$CXX" -fsanitize=address,undefined -x c++ - \
     -o "$OUT/.sancheck" 2>/dev/null; then
  rm -f "$OUT/.sancheck" "$OUT/.sancheck.exe"
  echo "=== sanitized build (address, undefined) ==="
  # shellcheck disable=SC2086
  "$CXX" -std=gnu++20 -O1 -g $WARN -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I"$COBS" $SRC -o "$OUT/test_decoder.exe"
else
  rm -f "$OUT/.sancheck" "$OUT/.sancheck.exe"
  echo "=== plain build (no sanitizer runtime in this toolchain) ==="
  # shellcheck disable=SC2086
  "$CXX" -std=gnu++20 -O1 -g $WARN -I"$COBS" $SRC -o "$OUT/test_decoder.exe"
fi

"$OUT/test_decoder.exe"
