#!/bin/bash
# 6.60: the sky's colours end to end without game data: gen_sky_data.py (Light.dbc + LightIntBand.dbc) -> w3dconv --texts-only (sky.txt) ->
# sky_test (the client's SkyTable: the global and zone lights, the day curve, the server's clock), then net/tests/clock_test (the
# server's time packets into GameSession::gameHour).
#   run_sky.sh <converter build dir with w3dconv>
set -e -o pipefail
B=${1:?converter build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_sky}; rm -rf "$OUT"; mkdir -p "$OUT/data" "$OUT/sd"
python3 "$T/../../converter/tests/gen_sky_data.py" "$OUT/data"
"$B/w3dconv" --data "$OUT/data" --texts-only -o "$OUT/sd/x.w3dp" > "$OUT/conv.log" 2>&1 || true
grep -E "sky:" "$OUT/conv.log"
grep -q "sky: 3 light volumes, 15 colour curves of 3 parameter sets" "$OUT/conv.log"
grep -c "^L" "$OUT/sd/sky.txt" | grep -qx 3
grep -qP "^B\t2\t5\r?$" "$OUT/sd/sky.txt"; grep -qP "^B\t3\t6\r?$" "$OUT/sd/sky.txt"; ! grep -qP "^B\t1\t" "$OUT/sd/sky.txt"
"$B/w3dconv" --data "$OUT/data" --skybox -o "$OUT/skybox.w3dp" > "$OUT/skybox.log" 2>&1
grep -q "sky models: 1 written (1 failed)" "$OUT/skybox.log"
python3 "$T/../../converter/tests/check_skybox.py" "$OUT/skybox.w3dp"
g++ -std=c++20 -g -O1 -Wall -fsanitize=address,undefined -I"$T/../source" "$T/sky_test.cpp" "$T/../source/sky.cpp" -o "$OUT/sky_test"
"$OUT/sky_test" "$OUT/sd/sky.txt"
N="$T/../../net"
SRC=$(ls "$N"/*.cpp | grep -v "/tools/")
g++ -std=c++20 -g -O1 -Wall -fsanitize=address,undefined -I"$N" "$N/tests/clock_test.cpp" $SRC -o "$OUT/clock_test" 2> "$OUT/clock_build.log" || { cat "$OUT/clock_build.log" | head -20; exit 1; }
"$OUT/clock_test"
