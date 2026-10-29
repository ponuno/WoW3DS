#!/bin/bash
# Stage 6: the perf log on the PC (perf_test.cpp): perf_core, then perflog.cpp over the streamer with stubbed libctru,
# under ASan+UBSan and TSan; then tools/perflog/analyze.py over the files it wrote.
#   run_perf.sh <pack.w3dp> [seconds]
set -e
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_perf}; mkdir -p "$OUT/asan" "$OUT/tsan"
SRC="$T/perf_test.cpp $T/../source/perflog.cpp $T/../source/fx.cpp $T/../source/streamer.cpp $T/stub/ctru_stub.cpp"
FL="-std=gnu++20 -g -O1 -DW3D_PC_TEST -Wall -Wno-deprecated-declarations -I$T/stub -I$T/../source -I$T/../../common"
g++ $FL -fsanitize=address,undefined $SRC -o "$OUT/perf_asan" -lpthread
g++ $FL -fsanitize=thread $SRC -o "$OUT/perf_tsan" -lpthread
"$OUT/perf_asan" "$1" "$OUT/asan" "${2:-6}"
rm -f "$OUT/asan_stall/perf_stall.txt"; mkdir -p "$OUT/asan_stall"
PERF_STALL=1 "$OUT/perf_asan" "$1" "$OUT/asan_stall" 9 | sed 's/^/stall run: /'   # stage 6.6: the freeze watchdog
"$OUT/perf_tsan" "$1" "$OUT/tsan" 4 2>&1 | grep -E "WARNING: ThreadSanitizer|PERF" | sort | uniq -c
python3 "$T/../../tools/perflog/analyze.py" "$OUT/asan/perf.csv" > "$OUT/analyze.txt" && echo "analyze.py: OK ($OUT/analyze.txt)"
