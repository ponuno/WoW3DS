#!/bin/bash
# The 3DS streamer (proto3ds/source/streamer.cpp) on the PC with stubbed libctru/citro3d (stub/):
# ASan+UBSan and TSan runs over a pack, with a shutdown in the middle of loading.
#   run_streamer.sh <pack.w3dp> [more packs...]
set -e
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_streamer}; mkdir -p "$OUT"
SRC="$T/streamer_pc.cpp $T/../source/fx.cpp $T/../source/streamer.cpp $T/../source/creature_view.cpp $T/stub/ctru_stub.cpp"
FL="-std=gnu++20 -g -O1 -DW3D_PC_TEST -I$T/stub -I$T/../source -I$T/../../common"
g++ $FL -fsanitize=address,undefined $SRC -o "$OUT/streamer_asan" -lpthread
g++ $FL -fsanitize=thread $SRC -o "$OUT/streamer_tsan" -lpthread
# WALK_EXPECT=<converter/tests expect.json> also walks the synthetic stairs through streamerFloorHeight()
# and the wall cases (hut, fences) through streamerCollideMove().
for p in "$@"; do
    for pace in 0 100 1000; do PACE_US=$pace "$OUT/streamer_asan" "$p" 1500 | grep -E "floor walk|wall|liquid|creatures:|players:|STREAMER"; done
    PACE_US=3000 "$OUT/streamer_tsan" "$p" 600 2>&1 | grep -E "WARNING: ThreadSanitizer|STREAMER" | sort | uniq -c
done
