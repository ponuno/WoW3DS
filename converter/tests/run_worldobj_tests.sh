#!/bin/bash
# Stage 5.41 converter test without game data: elevator / tram models with a deck, TransportAnimation.dbc -> "A" lines,
# the compact default object set (--creatures) and the door animations kept in the model.
#   run_worldobj_tests.sh <build dir with w3dconv>
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_worldobj_conv}
rm -rf "$OUT" && mkdir -p "$OUT/sd"
python3 "$T/gen_wmo_data.py" "$OUT/Data" "$OUT/wmo.json" > /dev/null
python3 "$T/gen_creature_data.py" "$OUT/Data" "$OUT/crt.json"
python3 "$T/gen_worldobj_data.py" "$OUT/Data" "$OUT/wo.json"
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 33 48 --transports --creatures --creature-displays 999999 -v -o "$OUT/sd/t.w3dp" 2>&1 | tee "$OUT/conv.log" | grep -E "transports|default game objects|WITHOUT a deck"
# 6.59c: Giant refused by its root box before its group is read, Wide by its collision box; the report counts both
grep -q "too big for an object (a city / dungeon, skipped): World.wmo.Test.Giant.wmo" "$OUT/conv.log" || { echo "FAIL: Giant not refused"; exit 1; }
grep -q "too big for an object (a city / dungeon, skipped): World.wmo.Test.Wide.wmo" "$OUT/conv.log" || { echo "FAIL: Wide not refused"; exit 1; }
if grep -qiE "WMO +World.wmo.Test.Giant.wmo +[0-9]" "$OUT/conv.log"; then echo "FAIL: Giant's groups were converted"; exit 1; fi
grep -q "(2 too big: cities / dungeons)" "$OUT/conv.log" || { echo "FAIL: report line"; grep "object WMOs" "$OUT/conv.log"; exit 1; }
python3 "$T/check_worldobj.py" "$OUT/sd/t.w3dp" "$OUT/sd/transports.txt" "$OUT/wo.json"
