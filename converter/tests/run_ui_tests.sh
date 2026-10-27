#!/bin/bash
# UI stage 7: w3dconv --ui on a synthetic client (gen_ui_data.py), then the 3DS-side reader + PC rasteriser check it
# (ui_pack_test.cpp: icons, 9-slice, arrows, fonts with the Cyrillic fallback, maps).
#   run_ui_tests.sh <w3dconv>
set -e
T=$(cd "$(dirname "$0")" && pwd); W=$(cd "$T/../.." && pwd)
CONV=${1:-$W/../../build/conv/w3dconv}
OUT=${OUT:-/tmp/w3d_uipack}
D=${DEPS:-/home/claude/deps}
LATIN=${LATIN_TTF:-/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf}
rm -rf "$OUT" && mkdir -p "$OUT/out"
python3 "$T/gen_ui_data.py" "$OUT/data" "$LATIN"
"$CONV" --data "$OUT/data" --ui -o "$OUT/out/ui.w3dp" | tail -8
"$CONV" --info "$OUT/out/ui.w3dp" | tail -2
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -I"$W/proto3ds/source" -I"$W/proto3ds/tests" -I"$W/common" -I"$W/converter/extern/stb" \
    -I"$D/rgetc1" "$W/proto3ds/tests/ui_pack_test.cpp" "$W/proto3ds/tests/ui_raster.cpp" "$W/proto3ds/source/hud.cpp" \
    "$W/proto3ds/source/ui_assets.cpp" "$D/rgetc1/rg_etc1.cpp" -w -o "$OUT/ui_pack_test"
"$OUT/ui_pack_test" "$OUT/out/ui.w3dp" "$OUT/ui_pack.png"
grep -q "^B" "$OUT/out/talents.txt" && grep -q "^N" "$OUT/out/taxi.txt" && echo "TEXTS (talents.txt, taxi.txt): PASS"
grep -q "^L	99	2	0	0	0	0	0	0	0	2	" "$OUT/out/locks.txt" && grep -q "^S	2366	2$" "$OUT/out/locks.txt" && echo "LOCKS (locks.txt): PASS"
