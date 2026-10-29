#!/bin/bash
# 6.18d: the packets of a fall off a ledge (Mover::fall / land).
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_net_fall}
mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$N" "$N/tests/fall_test.cpp" "$N/movement.cpp" -o "$OUT/fall_test"
"$OUT/fall_test"
