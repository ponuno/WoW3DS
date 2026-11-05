#!/bin/bash
# Stage 5.3-5.5 converter tests without game data: creatures (CDSP/CMDL) and players (CHRS/CIMG).
#   run_player_tests.sh <build dir with w3dconv/w3dpreview/creature_test>
#   1. gen_wmo_data.py (map) + gen_creature_data.py (wolf, human model, DBCs, CharSections layers with
#      alpha, faces, facial hair, underwear, hair; a row whose art is missing)
#   2. w3dconv --creatures --players; check_creatures.py (displays, NPC geosets, wolf sequences)
#   3. char_test: three looks resolved and composited with the 3DS code (common/w3d_character.h),
#      hair texture, geosets and all 16384 texels of each body texture against Python
#   4. creature_test (pose against WoWee), w3dpreview --player
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_players}
rm -rf "$OUT" && mkdir -p "$OUT/prev"
python3 "$T/gen_wmo_data.py" "$OUT/Data" "$OUT/wmo.json" > /dev/null
python3 "$T/gen_creature_data.py" "$OUT/Data" "$OUT/crt.json"
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 33 48 --creatures --players -o "$OUT/p.w3dp" | tee "$OUT/conv.log" | grep -E "creatures:|players:|items:|^OK"
grep -q "players: 1 race/sex models (15 failed), 8 CharSections rows, 7 layers (1 failed)" "$OUT/conv.log"
python3 "$T/check_creatures.py" "$OUT/p.w3dp" "$OUT/crt.json" | tail -1
python3 "$T/check_items.py" "$OUT/p.w3dp" "$OUT/crt.json" | tail -1   # worn items: ITEM / CIMG / MODL, attachments
python3 "$T/check_glue.py" "$OUT/p.w3dp" "$OUT/crt.json" "$OUT/glue.txt"   # stage 5.29: login / character scenes, glue.txt
# 6.35: every CMDL passes w3d_creature_valid (the client refuses the rest); a palette over 24 bones is refused
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I"$T/../../common" "$T/creature_valid_test.cpp" -o "$OUT/creature_valid_test"
"$OUT/creature_valid_test" "$OUT/p.w3dp" | tail -1
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$T/../../common" "$T/char_test.cpp" -o "$OUT/char_test"
"$OUT/char_test" "$OUT/p.w3dp" "$OUT/crt.json" "$OUT/prev"
"$B/creature_test" | tail -1
"$B/w3dpreview" "$OUT/p.w3dp" "$OUT/prev" --player 1 0 0 0 4 7 2 | head -1
"$B/w3dpreview" "$OUT/p.w3dp" "$OUT/prev" --player 1 0 0 0 4 7 2 --equip 4=7101,7=7002 | head -1 | grep -q "7 layers"   # worn armour in the preview
echo "PLAYER TESTS: PASS"
