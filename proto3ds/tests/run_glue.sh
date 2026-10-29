#!/bin/bash
# Stage 5.29: the login / realm / character screens on the PC (glue_ui.cpp + glue_data.cpp with the PC rasteriser):
# every screen drawn to $OUT/glue_*.png (+ glue_sheet.png) and driven with buttons and taps.
#   run_glue.sh [ui.w3dp] [glue.txt]      W3D_LANG=1: the Russian texts
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_glue}
mkdir -p "$OUT"
D=${DEPS:-/home/claude/deps}
SRC="$P/tests/glue_test.cpp $P/source/glue_ui.cpp $P/source/glue_data.cpp $P/source/hud.cpp $P/source/ui_assets.cpp \
     $P/source/ui_style.cpp $P/tests/ui_raster.cpp $D/rgetc1/rg_etc1.cpp"
INC="-I$P/tests/ui_stub -I$P/../net -I$P/source -I$P/../common -I$P/tests -I$P/../converter/extern/stb -I$D/rgetc1"
g++ -std=c++20 -O1 -c -w -I$D/rgetc1 $D/rgetc1/rg_etc1.cpp -o "$OUT/rg_etc1.o"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall $INC ${SRC/$D\/rgetc1\/rg_etc1.cpp/} "$OUT/rg_etc1.o" -o "$OUT/glue_test"
"$OUT/glue_test" "$OUT" "${1:-}" "${2:-}" "${W3D_LANG:-0}"
