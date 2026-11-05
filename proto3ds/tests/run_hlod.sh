#!/bin/bash
# 6.57: the horizon end to end without game data: the synthetic town (converter/tests/pvs_city_gen.cpp) -> w3docc --pvs and --hlod ->
# the client's hlod.cpp over the streamer (stubbed libctru, ASan + UBSan; the reader thread under TSan).
#   run_hlod.sh <dir with w3docc>
set -e -o pipefail
B=${1:?dir with w3docc}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_hlod}; rm -rf "$OUT"; mkdir -p "$OUT"
g++ -std=c++20 -O2 -I"$T/../../common" "$T/../../converter/tests/pvs_city_gen.cpp" -o "$OUT/pvs_city_gen"
"$OUT/pvs_city_gen" "$OUT/town.w3dp" > /dev/null
"$B/w3docc" --pvs "$OUT/town.w3dp" --around -8800 -266.7 240 --check 0 | grep -E "^pvs \(|built in"
"$B/w3docc" --hlod "$OUT/town.w3dp" | tee "$OUT/hlod.log"
grep -q "blocks of 133 yd" "$OUT/hlod.log"
SRC="$T/hlod_test.cpp $T/../source/hlod.cpp $T/../source/objcoll.cpp $T/../source/pvs.cpp $T/../source/fx.cpp $T/../source/streamer.cpp $T/../source/creature_view.cpp $T/stub/ctru_stub.cpp"
g++ -std=gnu++20 -g -O1 -DW3D_PC_TEST -I$T/stub -I$T/../source -I$T/../../common -fsanitize=address,undefined $SRC -o "$OUT/hlod_test" -lpthread 2> "$OUT/build.log" || { cat "$OUT/build.log"; exit 1; }
"$OUT/hlod_test" "$OUT/town.w3dp" -8800 -266.7
"$OUT/hlod_test" "$OUT/town.w3dp" -8600 -80
g++ -std=gnu++20 -g -O1 -DW3D_PC_TEST -I$T/stub -I$T/../source -I$T/../../common -fsanitize=thread $SRC -o "$OUT/hlod_tsan" -lpthread 2>> "$OUT/build.log"
"$OUT/hlod_tsan" "$OUT/town.w3dp" -8800 -266.7 2>&1 | grep -E "WARNING: ThreadSanitizer|HLOD CLIENT" | sort | uniq -c | tee "$OUT/tsan.log"
! grep -q ThreadSanitizer "$OUT/tsan.log"
echo "HLOD RUN: PASS"
