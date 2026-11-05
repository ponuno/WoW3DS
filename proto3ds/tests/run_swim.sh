#!/bin/bash
# Stage 5.26: swimming on the PC (swim_test.cpp: swim.cpp + the movement packets against the server's rules).
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_swim}; mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -I"$P/../common" -I"$P/../net" -I"$P/source" \
    "$P/tests/swim_test.cpp" "$P/source/swim.cpp" "$P/../net/movement.cpp" -o "$OUT/swim_test"
"$OUT/swim_test"
