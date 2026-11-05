#!/bin/bash
# 6.18c: slope limit of the terrain, step height, the shared wall/floor boundary (synthetic data, no pack).
set -e
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_slope}
mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$T/../../common" "$T/slope_test.cpp" -o "$OUT/slope_test"
"$OUT/slope_test"
