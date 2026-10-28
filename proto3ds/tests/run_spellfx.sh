#!/bin/bash
# Stage 5.33: spell effects. Synthetic Spell / SpellVisual / SpellVisualKit / SpellVisualEffectName data and effect models
# (converter/tests/gen_spellfx_data.py) on top of the building test world (gen_wmo_data.py):
#   w3dconv --spell-fx  -> the SVFX table and the effect models (checked by --info)
#   spellfx_test        -> the director: stages, both hands, missiles at the table's speed, impacts on arrival, fail,
#                          range, the limit, units leaving, lingering; auras, channels, loot sparkles, level-up (5.34)
#   spell_stream_test   -> with the streamer (stubbed libctru, ASan): models read, emitters running, all freed
#   run_spellfx.sh <build dir with w3dconv>
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
C=$T/../../converter/tests
OUT=${OUT:-/tmp/w3d_spellfx}
rm -rf "$OUT" && mkdir -p "$OUT"
python3 "$C/gen_wmo_data.py" "$OUT/Data" "$OUT/wmo_expect.json" > /dev/null
python3 "$C/gen_spellfx_data.py" "$OUT/Data" "$OUT/expect.json"
"$B/w3dconv" --data "$OUT/Data" --spell-fx -o "$OUT/sfx.w3dp" > "$OUT/sfx.log"
"$B/w3dconv" --info "$OUT/sfx.w3dp" | tee "$OUT/sfx_info.log" | grep -E "spell fx|effects|^OK"
grep -q "spell fx: 27 spells (13 with a missile speed), 5 visuals (2 with a missile), 11 effect models" "$OUT/sfx_info.log"
grep -q "spell fx: 1 auras, 1 channels, level-up yes, loot sparkle yes" "$OUT/sfx_info.log"   # stage 5.34
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 33 48 --gameobjects -o "$OUT/world.w3dp" > "$OUT/world.log" 2>&1
"$B/w3dconv" --info "$OUT/world.w3dp" | grep -q "spell fx: 27 spells"
FL="-std=gnu++20 -g -O1 -Wall -Wno-maybe-uninitialized -fsanitize=address,undefined -I$T/../../common -I$T/../source"
g++ $FL "$T/spellfx_test.cpp" "$T/../source/spellfx.cpp" -o "$OUT/spellfx_test"
"$OUT/spellfx_test" "$OUT/sfx.w3dp" "$OUT/expect.json"
g++ $FL -DW3D_PC_TEST -I"$T/stub" "$T/spell_stream_test.cpp" "$T/../source/spellfx.cpp" "$T/../source/fx.cpp" \
    "$T/../source/streamer.cpp" "$T/stub/ctru_stub.cpp" -o "$OUT/spell_stream_test" -lpthread
"$OUT/spell_stream_test" "$OUT/world.w3dp" | tail -2
echo "SPELL FX TESTS: PASS"
