#!/bin/bash
# Stage 5.26 (liquids) without game data. Usage: run_water_tests.sh <build dir with w3dconv/w3dpreview>
#   gen_water_data.py: two ADT tiles with MCLQ in the vanilla layout (several layers in one chunk, hidden cells, deep sea,
#   a slope), a building with two MLIQ groups (water, lava) placed turned and across the tile border, the animated
#   liquid textures (30 grey river frames, 5 blue sea frames, no lava), Light.dbc + bands (a global and a zone light)
#   and an instance map whose global building is the same pool.
#   liquid_test checks the LIQD query grid (every expected cell), the Light colours, the frames and the merged mesh;
#   then the dungeon pack, the validator, --no-wmo-water, --water-frames and the previews.
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_water}
rm -rf "$OUT" && mkdir -p "$OUT/prev" "$OUT/packs"
python3 "$T/gen_water_data.py" "$OUT/Data" "$OUT/expect.txt"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$T/../../common" "$T/liquid_test.cpp" -o "$OUT/liquid_test"
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 33 48 -o "$OUT/water.w3dp" -v > "$OUT/conv.log" 2>&1 || { tail -20 "$OUT/conv.log"; exit 1; }
grep -E "^  liquids:|texture XTextures" "$OUT/conv.log"
grep -q "liquids: 2 tiles, 7 MCLQ layers, 319 cells (64 deep), 12 building cells from 2 groups" "$OUT/conv.log"
grep -q "lake_a.1.blp: 32x32, mean rgb 144 144 144, saturation 0.00 -> Light.dbc tint 1.0" "$OUT/conv.log"
"$B/w3dconv" --info "$OUT/water.w3dp" | tee "$OUT/info.log" | grep -E "LIQD|WATR|water |liquid |^OK"
grep -q "^OK" "$OUT/info.log"
"$OUT/liquid_test" "$OUT/water.w3dp" "$OUT/expect.txt"
# the instance map: the pool and the lava pit of its building, in map43.w3dp
"$B/w3dconv" --data "$OUT/Data" --dungeons -o "$OUT/packs" > "$OUT/dungeon.log" 2>&1 || { tail -20 "$OUT/dungeon.log"; exit 1; }
"$B/w3dconv" --info "$OUT/packs/map43.w3dp" | grep -E "LIQD|^OK"
"$OUT/liquid_test" "$OUT/packs/map43.w3dp" "$OUT/expect_dungeon.txt"
# --no-wmo-water: the terrain's liquids only; --water-frames 10: every third frame (river 10, sea: frames 1 and 4)
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 33 48 -o "$OUT/nowmo.w3dp" --no-wmo-water --water-frames 10 > "$OUT/nowmo.log" 2>&1
grep -q "0 building cells from 0 groups" "$OUT/nowmo.log"
"$B/w3dconv" --info "$OUT/nowmo.w3dp" | grep -q "liquid 32_48: .* (deep 64, buildings 0)"
"$B/w3dconv" --info "$OUT/nowmo.w3dp" | grep -q "water 32_48: .* (10 frames)"
# previews: region.png shows the water in its colours, liquid_<x>_<y>.png the swimming grid
"$B/w3dpreview" "$OUT/water.w3dp" "$OUT/prev" > /dev/null
"$B/w3dpreview" "$OUT/water.w3dp" "$OUT/prev" --liquid | tee "$OUT/prev.log"
grep -q "liquid_32_48.png: 260 wet cells, surface 0.00 .. 81.00" "$OUT/prev.log"
echo "WATER TESTS: PASS"
