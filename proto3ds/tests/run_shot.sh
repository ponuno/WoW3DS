#!/bin/bash
# 6.95: arrows and bullets (Director::shoot) - runs anywhere with g++.
set -e
P=$(cd "$(dirname "$0")/.." && pwd); OUT=${OUT:-/tmp/w3d_shot}; mkdir -p "$OUT"
g++ -std=c++20 -Wall -fsanitize=address,undefined -I"$P/source" -I"$P/../common" "$P/tests/shot_test.cpp" "$P/source/spellfx.cpp" -o "$OUT/shot_test"
"$OUT/shot_test"
