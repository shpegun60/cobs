#!/bin/sh
# Author: shpegun60; SPDX-License-Identifier: MIT
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE"
while [ ! -f "$ROOT/COBS.pro" ]; do [ "$ROOT" != / ] || exit 1; ROOT="$(dirname "$ROOT")"; done
CXX="${CXX:-g++}"
OUT="$HERE/out/contracts"
mkdir -p "$OUT"
for header in Format.h Stats.h Tcp.h detail/RxBlock.h detail/Packet.h detail/Message.h detail/Receiver.h; do
    echo "#include \"modbus/tcp/$header\"" | "$CXX" -std=c++20 -Wall -Wextra -Werror \
        -I"$ROOT/src" -I"$ROOT/libs/delegate" -x c++ - -c -o "$OUT/header.o"
    echo "TCP self-contained header: $header"
done
for bad in 1 2 3 4 5 6 7 8 9; do
    if "$CXX" -std=c++20 -I"$ROOT/src" -I"$ROOT/libs/delegate" -DTCP_BAD=$bad \
        -c "$HERE/compile_fail.cpp" -o "$OUT/bad.o" > "$OUT/bad-$bad.log" 2>&1; then
        echo "expected rejection $bad compiled" >&2; exit 1
    fi
    case $bad in
        1|2|3) marker='TCP data plus CRC, unit and function must fit';;
        4) marker='wire::Storage contract';;
        5) marker='TCP Format CRC must satisfy';;
        6) marker='append_be';;
        7) marker='private';;
        8) marker='template arguments';;
        9) marker='CRC wire_size must leave room';;
    esac
    grep -q "$marker" "$OUT/bad-$bad.log" || { echo "wrong diagnostic for $bad" >&2; exit 1; }
    echo "TCP negative contract $bad: rejected with $marker"
done
