#!/bin/bash
# 6.70: the shortened model vertex shader gives what the old one gave: picasso (PICASSO=path) assembles both, vsfold_cases folds
# random draws with vs_fold.hpp, vsfold_check.py runs both shaders on them.
#   run_vsfold.sh <old model.v.pica>
set -e
T=$(cd "$(dirname "$0")" && pwd); S="$T/../source"
OUT=${OUT:-/tmp/w3d_vsfold}; mkdir -p "$OUT"
P=${PICASSO:-picasso}
"$P" -o "$OUT/old.shbin" "$1"
"$P" -o "$OUT/new.shbin" "$S/model.v.pica"
g++ -std=c++17 -O1 -Wall -I"$S" "$T/vsfold_cases.cpp" -o "$OUT/vsfold_cases"
"$OUT/vsfold_cases" > "$OUT/cases.txt"
python3 "$T/vsfold_check.py" "$OUT/old.shbin" "$OUT/new.shbin" "$OUT/cases.txt"
