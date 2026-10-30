#!/bin/bash
# 6.17: terrain holes (converter/tests/gen_terrain_layers.py: 3 chunks with holes) through the real streamer: the sample points
# inside holes are reported, the filled-in height over a hole is never a floor to step up onto, the ground beside it is.
#   run_holes.sh <build dir with w3dconv>
set -e
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_holes}
rm -rf "$OUT" && mkdir -p "$OUT"
python3 "$T/../../converter/tests/gen_terrain_layers.py" "$OUT/Data" > /dev/null
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 32 48 -o "$OUT/t.w3dp" > "$OUT/conv.log" 2>&1
SRC="$T/streamer_pc.cpp $T/../source/fx.cpp $T/../source/streamer.cpp $T/../source/creature_view.cpp $T/stub/ctru_stub.cpp"
g++ -std=gnu++20 -g -O1 -DW3D_PC_TEST -I$T/stub -I$T/../source -I$T/../../common -fsanitize=address,undefined $SRC -o "$OUT/holes" -lpthread
FAR_EXPECT=1 HOLE_EXPECT=14 "$OUT/holes" "$OUT/t.w3dp" 300 > "$OUT/holes.log" 2>&1 || true   # (an empty pack fails the model checks)
grep "terrain holes" "$OUT/holes.log"
grep -q "terrain holes: 14 uncovered points (membrane checks 14), ground checks [0-9]*, 0 wrong" "$OUT/holes.log"
echo "HOLES: PASS"
# 6.18a: the exact ground through the streamer, on uneven ground with holes (terrain_rough.py): with GRND within 0.01 yd of the
# brute-force reference (ground_ref.py); the same ground converted with --no-exact-ground is off by more than 0.2 yd (negative control)
python3 "$T/../../converter/tests/gen_terrain_layers.py" "$OUT/Rough" rough > /dev/null
python3 "$T/../../converter/tests/ground_ref.py" "$OUT/points.txt" | tail -1
"$B/w3dconv" --data "$OUT/Rough" --map Test --tiles 32 48 32 48 -o "$OUT/r.w3dp" > "$OUT/conv_r.log" 2>&1
"$B/w3dconv" --data "$OUT/Rough" --map Test --tiles 32 48 32 48 -o "$OUT/r_old.w3dp" --no-exact-ground > "$OUT/conv_r_old.log" 2>&1
GROUND_POINTS="$OUT/points.txt" GROUND_EXPECT=exact "$OUT/holes" "$OUT/r.w3dp" 300 > "$OUT/ground.log" 2>&1 || true
GROUND_POINTS="$OUT/points.txt" GROUND_EXPECT=old "$OUT/holes" "$OUT/r_old.w3dp" 300 > "$OUT/ground_old.log" 2>&1 || true
grep "ground via the streamer" "$OUT/ground.log" "$OUT/ground_old.log"
grep -q "ground via the streamer (GRND): [0-9]* points, worst error 0.00[0-9]* yd, 0 missing, 0 wrong hole flags" "$OUT/ground.log"
grep -q "ground via the streamer (old grid)" "$OUT/ground_old.log" && ! grep -q "ground FAIL" "$OUT/ground_old.log"
echo "GROUND (streamer): PASS"
# 6.24: the far ground through the streamer (converter/tests/gen_far_wdl.py: the converted tile + 2 WDL tiles): drawn for the
# tiles without detailed ground, never over one that has it, its buffers alive (ASan)
python3 "$T/../../converter/tests/gen_far_wdl.py" "$OUT/Rough"
"$B/w3dconv" --data "$OUT/Rough" --map Test --tiles 32 48 32 48 -o "$OUT/far.w3dp" > "$OUT/conv_far.log" 2>&1
FAR_EXPECT=3 "$OUT/holes" "$OUT/far.w3dp" 300 > "$OUT/far.log" 2>&1 || true
grep "beyond the detail distance (6.34c)" "$OUT/far.log"
grep -q "beyond the detail distance (6.34c): [1-9][0-9]* draws, 0 inside it" "$OUT/far.log"
grep "far ground" "$OUT/far.log"
grep -q "far ground: 3 tiles, drawn in [1-9][0-9]* frames, 0 wrong" "$OUT/far.log"
grep -q "far ground: 1 tiles, drawn in [0-9]* frames, 0 wrong" "$OUT/holes.log"
echo "FAR (streamer): PASS"
# 6.29: the far buildings through the streamer (converter/tests/gen_wmo_data.py, every building shelled): drawn while their tile is
# not shown, never from a tile that shows all its buildings, the buffers alive (ASan)
python3 "$T/../../converter/tests/gen_wmo_data.py" "$OUT/W" "$OUT/w.json" > /dev/null
"$B/w3dconv" --data "$OUT/W" --map Test --tiles 32 48 33 48 --far-obj-min 5 -o "$OUT/fo.w3dp" > "$OUT/conv_fo.log" 2>&1
FAROBJ_EXPECT=4 OWNED_EXPECT=2 "$OUT/holes" "$OUT/fo.w3dp" 600 > "$OUT/fo.log" 2>&1 || true
grep "far buildings" "$OUT/fo.log"
grep -q "beyond the detail distance (6.34): [1-9]" "$OUT/fo.log"
grep -q "far buildings: 4 in the map, drawn in [0-9]* frames, hidden behind the real ones in [1-9][0-9]*, 0 wrong" "$OUT/fo.log"   # (6.34: "drawn in" races the palette; beyond-distance draws are required instead)
echo "FAR BUILDINGS (streamer): PASS"
# 6.33: the house's two doodads (lamps; the barrel's model is missing) belong to it - shown only with it; the ADT's doodads do not
grep "wmo doodads owned" "$OUT/fo.log"
grep -q "WMO DOODAD OWNERS: PASS" "$OUT/fo.log"
echo "WMO DOODAD OWNERS (streamer): PASS"
