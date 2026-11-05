#!/bin/bash
# 6.54: the PVS end to end without game data: a synthetic walled town (converter/tests/pvs_city_gen.cpp) -> w3docc --pvs (its own
# check: random cameras, nothing seen may be culled) -> the client's pvs.cpp over the streamer (stubbed libctru, ASan + UBSan).
#   run_pvs.sh <dir with w3docc>
set -e -o pipefail
B=${1:?dir with w3docc}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_pvs}; rm -rf "$OUT"; mkdir -p "$OUT"
g++ -std=c++20 -O2 -I"$T/../../common" "$T/../../converter/tests/pvs_city_gen.cpp" -o "$OUT/pvs_city_gen"
"$OUT/pvs_city_gen" "$OUT/town.w3dp"
"$B/w3docc" --pvs "$OUT/town.w3dp" --around -8800 -266.7 240 --check 3000 | grep -v "eyes [0-9]*%" | tee "$OUT/pvs.log"
# the build's check: at most 1% of the views may miss anything (on this town: cameras on the flat roofs looking into a courtyard)
miss=$(grep -oE "views with something missing: [0-9]+ of [0-9]+ with the camera up to 4 yd over its floor, [0-9]+" "$OUT/pvs.log" | grep -oE "[0-9]+" | sed -n '1p;3p' | paste -sd+ | bc)
[ "$miss" -le 30 ] || { echo "PVS: $miss views missing something"; exit 1; }
grep -q "would draw" "$OUT/pvs.log"
SRC="$T/pvs_test.cpp $T/../source/pvs.cpp $T/../source/fx.cpp $T/../source/streamer.cpp $T/../source/hlod.cpp $T/../source/objcoll.cpp $T/../source/creature_view.cpp $T/stub/ctru_stub.cpp"
g++ -std=gnu++20 -g -O1 -DW3D_PC_TEST -I$T/stub -I$T/../source -I$T/../../common -fsanitize=address,undefined $SRC -o "$OUT/pvs_test" -lpthread 2> "$OUT/build.log" || { cat "$OUT/build.log"; exit 1; }
"$OUT/pvs_test" "$OUT/town.w3dp" -8908 -266.7     # a street crossing
"$OUT/pvs_test" "$OUT/town.w3dp" -8780 -180.0     # another spot
# 6.54c: the reader thread under TSan
g++ -std=gnu++20 -g -O1 -DW3D_PC_TEST -I$T/stub -I$T/../source -I$T/../../common -fsanitize=thread $SRC -o "$OUT/pvs_tsan" -lpthread 2>> "$OUT/build.log"
"$OUT/pvs_tsan" "$OUT/town.w3dp" -8780 -180.0 2>&1 | grep -E "WARNING: ThreadSanitizer|PVS CLIENT" | sort | uniq -c | tee "$OUT/tsan.log"
! grep -q ThreadSanitizer "$OUT/tsan.log"
# 6.54d: a town inside a 120-yd wall: nothing past it can be seen - the far ground / buildings skipped, never wrongly
mkdir -p "$OUT/tall"
"$OUT/pvs_city_gen" "$OUT/tall/town.w3dp" tall
"$B/w3docc" --pvs "$OUT/tall/town.w3dp" --around -8800 -266.7 150 --check 1500 | grep -v "eyes [0-9]*%" | tee "$OUT/tall.log"
grep -E "far ground / far buildings skipped in [1-9][0-9]* of" "$OUT/tall.log" > /dev/null || { echo "PVS: far never skipped in the walled town"; exit 1; }
grep -E "would show although skipped: 0" "$OUT/tall.log" > /dev/null || { echo "PVS: far skipped where something past the distance shows"; exit 1; }
"$OUT/pvs_test" "$OUT/tall/town.w3dp" -8908 -266.7 | tee "$OUT/tall_client.log"
grep -q "far hidden" "$OUT/tall_client.log" || grep -q ", far hidden;" "$OUT/tall_client.log"
# 6.54e: the walled town with a 300-yd peak ~1000 yd out (in a neighbour tile's far corner): the far ground's height per 133-yd block
# (the exact heights, GRND) - a height per tile put the peak at the draw distance and the far was never skipped
mkdir -p "$OUT/peak"
"$OUT/pvs_city_gen" "$OUT/peak/town.w3dp" tall peak
"$B/w3docc" --pvs "$OUT/peak/town.w3dp" --around -8800 -266.7 150 --check 1500 | grep -v "eyes [0-9]*%" | tee "$OUT/peak.log"
grep -E "far ground / far buildings skipped in [1-9][0-9]* of" "$OUT/peak.log" > /dev/null || { echo "PVS: far never skipped with the peak"; exit 1; }
grep -E "would show although skipped: 0" "$OUT/peak.log" > /dev/null || { echo "PVS: far skipped where something past the distance shows (peak)"; exit 1; }
echo "PVS RUN: PASS"
