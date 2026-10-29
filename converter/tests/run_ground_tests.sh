#!/bin/bash
# 6.18a: the exact ground (GRND). Usage: run_ground_tests.sh <build dir with w3dconv>
#   gen_terrain_layers.py rough -> w3dconv -> GRND entry -> w3d_ground_at() against the brute-force reference (ground_ref.py),
#   holes included; negative control: ground_ref.py asserts that the old corner-only field is off by > 0.2 yd on this ground;
#   --no-exact-ground leaves the entry out; --terrain-centers draws the centres (vertex / triangle counts).
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_ground}
rm -rf "$OUT" && mkdir -p "$OUT"
python3 "$T/gen_terrain_layers.py" "$OUT/Data" rough > /dev/null
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 32 48 -o "$OUT/g.w3dp" > "$OUT/conv.log" 2>&1 || { tail -20 "$OUT/conv.log"; exit 1; }
"$B/w3dconv" --info "$OUT/g.w3dp" | tee "$OUT/info.txt" | grep -E "ground|^OK"
grep -q "ground 32_48: 256 of 256 chunks, 3 with holes" "$OUT/info.txt"
python3 "$T/ground_ref.py" "$OUT/points.txt"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$T/../../common" "$T/ground_test.cpp" -o "$OUT/ground_test"
"$OUT/ground_test" "$OUT/g.w3dp" "$OUT/points.txt"
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 32 48 -o "$OUT/n.w3dp" --no-exact-ground > /dev/null 2>&1
if "$B/w3dconv" --info "$OUT/n.w3dp" | grep -q "ground 32_48"; then echo "--no-exact-ground still wrote GRND"; exit 1; fi
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 32 48 -o "$OUT/c.w3dp" --terrain-centers > "$OUT/conv_c.log" 2>&1 || { tail -20 "$OUT/conv_c.log"; exit 1; }
"$B/w3dconv" --info "$OUT/c.w3dp" | grep -q "^OK"
"$OUT/ground_test" "$OUT/c.w3dp" "$OUT/points.txt" | grep -q "GROUND: PASS"
V0=$(grep -oE "[0-9]+ terrain verts" "$OUT/conv.log" | head -1 | grep -oE "^[0-9]+" || true)
echo "terrain stats: default: $(grep -E "terrain" "$OUT/conv.log" | head -1)"
echo "               centres: $(grep -E "terrain" "$OUT/conv_c.log" | head -1)"
echo "GROUND TESTS: PASS"
