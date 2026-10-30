#!/bin/bash
# 6.24: the far ground (FARL). Usage: run_far_tests.sh <build dir with w3dconv>
#   gen_terrain_layers.py rough + gen_far_wdl.py -> w3dconv -> far_test (heights against GRND, the WDL tiles, the atlas,
#   the client's vertex / index builders); --no-far-lod leaves the entry out.
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_far}
rm -rf "$OUT" && mkdir -p "$OUT"
python3 "$T/gen_terrain_layers.py" "$OUT/Data" rough > /dev/null
python3 "$T/gen_far_wdl.py" "$OUT/Data"
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 32 48 -o "$OUT/f.w3dp" > "$OUT/conv.log" 2>&1 || { tail -20 "$OUT/conv.log"; exit 1; }
grep "far ground" "$OUT/conv.log"
grep -q "reading 0 (\[y\]\[x\] index, plain grid)" "$OUT/conv.log"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$T/../../common" "$T/far_test.cpp" -o "$OUT/far_test"
"$OUT/far_test" "$OUT/f.w3dp"
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 32 48 -o "$OUT/n.w3dp" --no-far-lod > /dev/null 2>&1
if grep -q "far ground" <("$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 32 48 -o "$OUT/n.w3dp" --no-far-lod 2>&1); then echo "--no-far-lod still wrote FARL"; exit 1; fi
"$B/w3dconv" --info "$OUT/f.w3dp" | grep -q "^OK"
echo "FAR TESTS: PASS"
