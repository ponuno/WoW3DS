#!/bin/bash
# Stage 5.24 (dungeons) without game data. Usage: run_dungeon_tests.sh <build dir with w3dconv>
#   gen_wmo_data.py + gen_dungeon_data.py: an instance map Crypt (WDT with one global building) and an ordinary map
#   w3dconv --dungeons must write map36.w3dp only (Map.dbc id 36), with terrain+placements+collision for the tiles the
#   building touches; --map Crypt must give the same through the normal path.
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_dungeon}
rm -rf "$OUT" && mkdir -p "$OUT/packs"
python3 "$T/gen_wmo_data.py" "$OUT/Data" "$OUT/expect.json" > /dev/null
python3 "$T/gen_dungeon_data.py" "$OUT/Data"
"$B/w3dconv" --data "$OUT/Data" --dungeons -o "$OUT/packs" -v | tee "$OUT/conv.log" | grep -E "dungeon|wrote|done"
test -s "$OUT/packs/map36.w3dp"
test "$(ls "$OUT/packs"/*.w3dp | wc -l)" = 1
grep -q "done: 1 of 1 dungeon packs" "$OUT/conv.log"
"$B/w3dconv" --info "$OUT/packs/map36.w3dp" | tee "$OUT/info.log" | grep -E "TERR|PLCM|COLL|MODL|tile"
grep -qE "TERR" "$OUT/info.log" && grep -qE "PLCM" "$OUT/info.log" && grep -qE "COLL" "$OUT/info.log"
"$B/w3dconv" --data "$OUT/Data" --map Crypt -o "$OUT/single.w3dp" > "$OUT/single.log"
grep -q "is an instance map" "$OUT/single.log"
diff <("$B/w3dconv" --info "$OUT/single.w3dp" | grep -E "^  (tile|TERR|PLCM|COLL)") <(grep -E "^  (tile|TERR|PLCM|COLL)" "$OUT/info.log")   # same tiles through --map
# a building at (0,0,0) (the real WDTs): the same tiles around the middle of the map, nothing in the corner tile 0_0
mkdir -p "$OUT/zero" && cp -r "$OUT/Data" "$OUT/zero/Data" && DUNGEON_ZERO=1 python3 "$T/gen_dungeon_data.py" "$OUT/zero/Data"
"$B/w3dconv" --data "$OUT/zero/Data" --map Crypt -o "$OUT/zero.w3dp" > "$OUT/zero.log"
"$B/w3dconv" --info "$OUT/zero.w3dp" > "$OUT/zero_info.log"
! grep -q "tile 0_0" "$OUT/zero_info.log"
diff <(grep -E "^  tile" "$OUT/zero_info.log") <(grep -E "^  tile" "$OUT/info.log")
echo "zero-position building: same tiles"
# in-game loading screens: ui.w3dp must carry one picture per map (the column of Map.dbc is found by itself)
"$B/w3dconv" --data "$OUT/Data" --ui -o "$OUT/ui.w3dp" > "$OUT/ui.log" 2>&1 || { tail -20 "$OUT/ui.log"; exit 1; }
grep "loading screens" "$OUT/ui.log"
grep -q "loading screens: 6 maps (Map.dbc column 38), 0 pictures failed" "$OUT/ui.log"
echo OK
