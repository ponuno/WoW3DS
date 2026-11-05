#!/bin/bash
# 6.71 / 6.72: the frame pacer and the automatic draw distance (pacer.hpp, autodetail.hpp) - decisions only (no 3DS code).
set -e
T=$(cd "$(dirname "$0")" && pwd); S="$T/../source"; OUT=${OUT:-/tmp/w3d_pacing}; mkdir -p "$OUT"
g++ -std=c++17 -Wall -fsanitize=address,undefined -I"$S" "$T/pacer_test.cpp" -o "$OUT/pacer_test" && "$OUT/pacer_test"
g++ -std=c++17 -Wall -fsanitize=address,undefined -I"$S" "$T/autodetail_test.cpp" -o "$OUT/autodetail_test" && "$OUT/autodetail_test"
