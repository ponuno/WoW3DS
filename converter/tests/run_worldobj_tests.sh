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
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 33 48 --transports --creatures --creature-displays 999999 -o "$OUT/sd/t.w3dp" | tee "$OUT/conv.log" | grep -E "transports|default game objects|WITHOUT a deck"
python3 "$T/check_worldobj.py" "$OUT/sd/t.w3dp" "$OUT/sd/transports.txt" "$OUT/wo.json"
