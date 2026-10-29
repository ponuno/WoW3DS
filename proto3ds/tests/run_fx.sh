#!/bin/bash
# Stage 5.32: particles and lamp glows. The core (source/fx.cpp) on its own, then with the streamer over the synthetic
# world of converter/tests/run_wmo_tests.sh (a lamp with a flame emitter and a glow card), freeing models while their
# particles live (ASan).   run_fx.sh [wmo.w3dp]
set -e
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_fx}; mkdir -p "$OUT"
PACK=${1:-/tmp/w3d_wmo/wmo.w3dp}
g++ -std=gnu++20 -g -O1 -Wall -fsanitize=address,undefined -I"$T/../../common" -I"$T/../source" "$T/fx_test.cpp" \
    "$T/../source/fx.cpp" -o "$OUT/fx_test"
"$OUT/fx_test"
SRC="$T/streamer_pc.cpp $T/../source/fx.cpp $T/../source/streamer.cpp $T/../source/creature_view.cpp $T/stub/ctru_stub.cpp"
g++ -std=gnu++20 -g -O1 -DW3D_PC_TEST -I"$T/stub" -I"$T/../source" -I"$T/../../common" -fsanitize=address,undefined $SRC \
    -o "$OUT/streamer_fx" -lpthread
for pace in 0 300; do PACE_US=$pace "$OUT/streamer_fx" "$PACK" 1500 | tee "$OUT/streamer_$pace.log" | grep -E "effects:|STREAMER"; done
grep -q "effects: [1-9][0-9]* models with effects, peak [1-9]" "$OUT/streamer_0.log"
grep -q "STREAMER: PASS" "$OUT/streamer_300.log"
echo "FX TESTS: PASS"
