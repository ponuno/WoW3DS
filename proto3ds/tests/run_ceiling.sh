#!/bin/bash
# 6.42: ceilings over a jumping walker (w3d_ceiling_above / w3d_rise_under_ceiling), hand-built COLL tile
set -e
T=$(cd "$(dirname "$0")" && pwd)
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I"$T/../../common" "$T/ceiling_test.cpp" -o "${TMPDIR:-/tmp}/ceiling_test"
"${TMPDIR:-/tmp}/ceiling_test"
