#!/bin/bash
# Stage 5.41: rotation matrices of tilted objects and the closed-door blockers (common/w3d_worldobj.h).
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_worldobj}; mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -Wextra "$P/tests/worldobj_test.cpp" -o "$OUT/worldobj_test"
"$OUT/worldobj_test"
