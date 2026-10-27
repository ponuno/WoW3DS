#!/bin/bash
# Stage 5.21 converter test without game data: w3dconv --transports on a synthetic ship (collision deck) + TaxiPathNode.dbc.
#   run_transport_tests.sh <build dir with w3dconv>
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_transports}
rm -rf "$OUT" && mkdir -p "$OUT/sd"
python3 "$T/gen_wmo_data.py" "$OUT/Data" "$OUT/wmo.json" > /dev/null
python3 "$T/gen_creature_data.py" "$OUT/Data" "$OUT/crt.json"
python3 "$T/gen_transport_data.py" "$OUT/Data" "$OUT/tr.json"
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 33 48 --transports -o "$OUT/sd/t.w3dp" | tee "$OUT/conv.log" | grep -E "transports"
python3 "$T/check_transports.py" "$OUT/sd/t.w3dp" "$OUT/sd/transports.txt" "$OUT/tr.json"
