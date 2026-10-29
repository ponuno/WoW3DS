#!/bin/bash
# Stage 5.0 converter tests without game data. Usage: run_wmo_tests.sh <build dir with w3dconv/w3dpreview>
#   1. gen_wmo_data.py: synthetic ADTs (MODF listed by two tiles, MDDF duplicate) + WMOs (exterior/interior,
#      doodad sets with quaternions, one group with reversed winding) + BLP texture
#   2. w3dconv over the region, --dump-placements; check_wmo_dump.py compares positions/rotations/scales
#      with the maths done independently in Python; check_pack_models.py checks indices and winding in the pack
#   3. coll_test: floor heights (ramp across a tile border, two floors, a detail face that must not count),
#      walls (hut with a door, M2 fence with collision, fence across a tile border, jump over it),
#      random walks checked independently by check_walls.py
#   4. w3dpreview draws the buildings into region.png
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_wmo}
rm -rf "$OUT" && mkdir -p "$OUT/prev"
python3 "$T/gen_wmo_data.py" "$OUT/Data" "$OUT/expect.json"
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 33 48 -o "$OUT/wmo.w3dp" --dump-placements "$OUT/dump.txt" | tee "$OUT/conv.log" | grep -E "buildings|instances|floors|collision|COLL|winding|duplicate|invisible|^OK"
grep -q "4 duplicate placements skipped" "$OUT/conv.log"   # house, stairs, lamp, border fence
grep -q "1 WMO submeshes flipped" "$OUT/conv.log"
grep -q "2 models with" "$OUT/conv.log" && grep -q "collision (0 simplified)" "$OUT/conv.log"   # 6.18b: no limit on doodad collision triangles any more (the fence keeps all 432); the invisible wall
grep -q "1 of them invisible walls" "$OUT/conv.log"   # 6.14: no drawn batch, still solid
python3 "$T/check_wmo_dump.py" "$OUT/dump.txt" "$OUT/expect.json"
python3 "$T/check_pack_models.py" "$OUT/wmo.w3dp"
"$B/w3dconv" --info "$OUT/wmo.w3dp" | grep -E "floors|^OK"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$T/../../common" "$T/coll_test.cpp" -o "$OUT/coll_test"
"$OUT/coll_test" "$OUT/wmo.w3dp" "$OUT/expect.json" "$OUT/walks.txt" | grep -E "FAIL|FLOORS|wall case|random|WALLS"
python3 "$T/check_walls.py" "$OUT/expect.json" "$OUT/walks.txt"
# --no-walls: floors only, every wall case except the free ones must now fail to stop
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 33 48 -o "$OUT/nowalls.w3dp" --no-walls > "$OUT/nowalls.log"
"$B/w3dconv" --info "$OUT/nowalls.w3dp" | grep -q "floors+walls 32_48: [0-9]* triangles (0 walls"
# negative controls: without walls the 8 blocking cases fail and the independent walk check must catch it
"$OUT/coll_test" "$OUT/nowalls.w3dp" "$OUT/expect.json" "$OUT/walks_nowalls.txt" > "$OUT/nowalls_test.txt" || true
grep -E "FLOORS: PASS" "$OUT/nowalls_test.txt"
grep -q "WALLS: FAIL (11 cases, 8 failed)" "$OUT/nowalls_test.txt"
if python3 "$T/check_walls.py" "$OUT/expect.json" "$OUT/walks_nowalls.txt" > "$OUT/nowalls_check.txt"; then
  echo "check_walls.py did not notice missing walls"; exit 1
fi
echo "negative control (--no-walls): $(tail -1 "$OUT/nowalls_check.txt")"
"$B/w3dpreview" "$OUT/wmo.w3dp" "$OUT/prev"
"$B/w3dpreview" "$OUT/wmo.w3dp" "$OUT/prev" --coll | grep -q "coll_32_48.png: .* walls"
# stage 5.31: simplified building groups keep every wall closed (no cracks between panels)
if [ -x "$B/wmo_hole_test" ]; then "$B/wmo_hole_test"; fi
echo "WMO TESTS: PASS"
