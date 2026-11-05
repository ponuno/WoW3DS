#!/bin/bash
# 6.59: game objects that are buildings, end to end without game data: converter/tests/run_worldobj_tests.sh (the synthetic house as
# GameObjectDisplayInfo 4009 -> an OWMO entry) -> objwmo_test over the streamer (stubbed libctru, ASan + UBSan): parts loaded, the
# house placed turned and scaled, floor / walls / ceiling through the walker's queries.
#   run_objwmo.sh <converter build dir with w3dconv>
set -e -o pipefail
B=${1:?converter build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_objwmo}; rm -rf "$OUT"; mkdir -p "$OUT"
OUT="$OUT/conv" bash "$T/../../converter/tests/run_worldobj_tests.sh" "$B" | tail -1
SRC="$T/objwmo_test.cpp $T/../source/objcoll.cpp $T/../source/hlod.cpp $T/../source/fx.cpp $T/../source/streamer.cpp $T/../source/creature_view.cpp $T/stub/ctru_stub.cpp"
g++ -std=gnu++20 -g -O1 -DW3D_PC_TEST -I$T/stub -I$T/../source -I$T/../../common -fsanitize=address,undefined $SRC -o "$OUT/objwmo_test" -lpthread 2> "$OUT/build.log" || { cat "$OUT/build.log"; exit 1; }
"$OUT/objwmo_test" "$OUT/conv/sd/t.w3dp"
