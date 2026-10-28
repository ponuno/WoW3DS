#!/bin/bash
# Stage 5.27: the shared asset pool and 'w3dconv --all', without game data. Usage: run_pool_tests.sh <build dir>
#   gen_water_data.py's world (map 0 "Test": two tiles, map 43 "Grotto": an instance sharing the building) converted
#   (a) one pack per map, the old way, and (b) with --all into map0/map43 + common.w3dp: the same pictures and liquid
#   queries, fewer bytes (the shared building, its textures and the liquid frames stored once); the pool split into
#   parts (--pool-part-kb); the validator and the 3DS streamer (PC build) resolving the pool; a map pack whose pool is
#   from another conversion refused by both.
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
P=$T/../../proto3ds/tests
OUT=${OUT:-/tmp/w3d_pool}
rm -rf "$OUT" && mkdir -p "$OUT/single" "$OUT/all" "$OUT/parts" "$OUT/mixed" "$OUT/prev1" "$OUT/prev2"
python3 "$T/gen_water_data.py" "$OUT/Data" "$OUT/expect.txt"
[ -x "$B/pool_test" ] && "$B/pool_test"
# (a) separate packs
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 0 0 63 63 -o "$OUT/single/map0.w3dp" > "$OUT/single.log" 2>&1 || { tail -20 "$OUT/single.log"; exit 1; }
"$B/w3dconv" --data "$OUT/Data" --dungeons -o "$OUT/single" > "$OUT/single_d.log" 2>&1 || { tail -20 "$OUT/single_d.log"; exit 1; }
# (b) everything, one pool
"$B/w3dconv" --data "$OUT/Data" --all --no-ui --no-sound-step -o "$OUT/all" > "$OUT/all.log" 2>&1 || { tail -30 "$OUT/all.log"; exit 1; }
grep -E "^all:|^  map|^  pool:" "$OUT/all.log"
ls "$OUT/all"/map0.w3dp "$OUT/all"/map43.w3dp "$OUT/all"/common.w3dp > /dev/null
[ ! -e "$OUT/all/common1.w3dp" ]
[ -z "$(ls "$OUT/all"/*.spill 2>/dev/null)" ]
for f in map0 map43; do
    "$B/w3dconv" --info "$OUT/all/$f.w3dp" > "$OUT/info_$f.log"
    grep -E "PINF:|^OK" "$OUT/info_$f.log"
    grep -q "^OK" "$OUT/info_$f.log"
done
"$B/w3dconv" --info "$OUT/all/common.w3dp" | grep -q "^OK"
sz() { cat "$@" | wc -c; }
S1=$(sz "$OUT/single/map0.w3dp" "$OUT/single/map43.w3dp")
S2=$(sz "$OUT/all/map0.w3dp" "$OUT/all/map43.w3dp" "$OUT/all/common.w3dp")
echo "separate packs: $S1 bytes, shared pool: $S2 bytes"
[ "$S2" -lt "$S1" ]
# the same world: liquid queries, and the region picture identical to the separate pack's
g++ -std=c++20 -O1 -g -I"$T/../../common" "$T/liquid_test.cpp" -o "$OUT/liquid_test"
"$B/w3dpreview" "$OUT/single/map0.w3dp" "$OUT/prev1" > /dev/null
"$B/w3dpreview" "$OUT/all/map0.w3dp" "$OUT/prev2" | grep "^pool:"
cmp "$OUT/prev1/region.png" "$OUT/prev2/region.png"
# (c) the pool in parts
"$B/w3dconv" --data "$OUT/Data" --all --no-ui --no-sound-step --pool-part-kb 16 -o "$OUT/parts" > "$OUT/parts.log" 2>&1 || { tail -30 "$OUT/parts.log"; exit 1; }
ls "$OUT/parts"/common1.w3dp > /dev/null
for f in "$OUT/parts"/common*.w3dp; do [ "$(sz "$f")" -le $((16 * 1024 + 65536)) ]; done
"$B/w3dconv" --info "$OUT/parts/map0.w3dp" | grep -q "^OK"
"$B/w3dconv" --info "$OUT/parts/common1.w3dp" | grep -E "PINF:"
# the 3DS streamer on the PC: models and textures from the pool (parts), liquids
SRC="$P/streamer_pc.cpp $P/../source/streamer.cpp $P/../source/creature_view.cpp $P/stub/ctru_stub.cpp"
g++ -std=gnu++20 -g -O1 -I"$P/stub" -I"$P/../source" -I"$T/../../common" -fsanitize=address,undefined $SRC -o "$OUT/streamer" -lpthread
for d in all parts; do
    LIQUID_EXPECT="$OUT/expect.txt" "$OUT/streamer" "$OUT/$d/map0.w3dp" 1500 | tee "$OUT/streamer_$d.log" | grep -E "pool|liquid|STREAMER"
    grep -q "STREAMER: PASS" "$OUT/streamer_$d.log"
done
# (d) a map pack with the pool of another conversion: refused
cp "$OUT/all/map0.w3dp" "$OUT/mixed/"
cp "$OUT/parts"/common*.w3dp "$OUT/mixed/"
if "$B/w3dconv" --info "$OUT/mixed/map0.w3dp" > "$OUT/mixed_info.log" 2>&1; then echo "validator accepted a foreign pool"; exit 1; fi
grep -E "another conversion" "$OUT/mixed_info.log"
if "$OUT/streamer" "$OUT/mixed/map0.w3dp" 10 > "$OUT/mixed_streamer.log" 2>&1; then echo "streamer accepted a foreign pool"; exit 1; fi
grep -E "^init:" "$OUT/mixed_streamer.log"
rm "$OUT/mixed"/common*.w3dp
if "$OUT/streamer" "$OUT/mixed/map0.w3dp" 10 > "$OUT/missing_streamer.log" 2>&1; then echo "streamer opened a map pack without its pool"; exit 1; fi
grep -E "^init:" "$OUT/missing_streamer.log"
# (e) creatures, players, worn items in the pool: the client must draw exactly what a standalone pack gives (stage 5.28 fix:
# displays and item displays were read from the map pack instead of the pool file)
mkdir -p "$OUT/cr/all"
python3 "$T/gen_wmo_data.py" "$OUT/cr/Data" "$OUT/cr/wmo.json" > /dev/null
python3 "$T/gen_creature_data.py" "$OUT/cr/Data" "$OUT/cr/crt.json" > /dev/null
cp "$OUT/Data/DBFilesClient/Map.dbc" "$OUT/cr/Data/DBFilesClient/Map.dbc"
"$B/w3dconv" --data "$OUT/cr/Data" --map Test --tiles 32 48 33 48 --creatures --players -o "$OUT/cr/single.w3dp" > /dev/null 2>&1
"$B/w3dconv" --data "$OUT/cr/Data" --all --no-ui --no-sound-step -o "$OUT/cr/all" > "$OUT/cr/all.log" 2>&1 || { tail -20 "$OUT/cr/all.log"; exit 1; }
"$B/w3dconv" --info "$OUT/cr/all/common.w3dp" | grep -E "CDSP|CMDL|CHRS|ITEM"
"$OUT/streamer" "$OUT/cr/single.w3dp" 1500 > "$OUT/cr/s1.log" 2>&1 || true
"$OUT/streamer" "$OUT/cr/all/map0.w3dp" 1500 > "$OUT/cr/s2.log" 2>&1 || true
grep -E "creatures:|players:|worn items|not in the pack|STREAMER" "$OUT/cr/s2.log"
grep -q "STREAMER: PASS" "$OUT/cr/s2.log"
# animations played and armour drawn: the same kinds as from the standalone pack
for k in "worn items: [1-9]" "seq death [0-9]+ walk [1-9]" "players: [1-9]"; do
    grep -Eq "$k" "$OUT/cr/s1.log" && grep -Eq "$k" "$OUT/cr/s2.log"
done
echo "POOL TESTS: PASS"
