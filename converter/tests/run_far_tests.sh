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
# 6.29: far buildings (FARO) on gen_wmo_data.py's world: the house and the tower (20 yd) by default, every building with a
# lower threshold; --no-far-objects leaves the entry out
python3 "$T/gen_wmo_data.py" "$OUT/W/Data" "$OUT/W/expect.json" > /dev/null
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$T/../../common" "$T/farobj_test.cpp" -o "$OUT/farobj_test"
"$B/w3dconv" --data "$OUT/W/Data" --map Test --tiles 32 48 33 48 -o "$OUT/W/o.w3dp" > "$OUT/W/conv.log" 2>&1 || { tail -20 "$OUT/W/conv.log"; exit 1; }
grep "far buildings" "$OUT/W/conv.log"
"$OUT/farobj_test" "$OUT/W/o.w3dp" 2
"$B/w3dconv" --data "$OUT/W/Data" --map Test --tiles 32 48 33 48 -o "$OUT/W/all.w3dp" --far-obj-min 5 > "$OUT/W/conv5.log" 2>&1
grep "far buildings" "$OUT/W/conv5.log"
"$OUT/farobj_test" "$OUT/W/all.w3dp" "$(grep -o 'far buildings: [0-9]*' "$OUT/W/conv5.log" | grep -o '[0-9]*$')"
grep -q "far buildings: [4-9]" "$OUT/W/conv5.log"
"$B/w3dconv" --info "$OUT/W/o.w3dp" | grep -q "^OK"
"$B/w3dconv" --data "$OUT/W/Data" --map Test --tiles 32 48 33 48 -o "$OUT/W/none.w3dp" --no-far-objects > /dev/null 2>&1
"$OUT/farobj_test" "$OUT/W/none.w3dp" 0
echo "FAR TESTS: PASS"
