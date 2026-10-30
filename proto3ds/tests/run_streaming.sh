#!/bin/bash
# Stage 5.30: streaming order on a synthetic city (w3dconv --synthetic --synthetic-variety: 9 tiles, ~1100 distinct models
# and textures) with the reads as slow as an SD card (SD_KBPS, SD_SEEK_US). Checks that the ground and what stands near
# the camera show first (ORDER_EXPECT), that reads of tiles left behind are dropped, no leaks (ASan) and no races (TSan).
#   run_streaming.sh <build dir with w3dconv>
set -e
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_streaming}
mkdir -p "$OUT"
"$B/w3dconv" --synthetic --tiles 31 47 33 49 --synthetic-variety 120 -o "$OUT/city.w3dp" > "$OUT/city.log" 2>&1
OUT="$OUT" bash "$T/run_streamer.sh" "$OUT/city.w3dp" > "$OUT/streamer.log" 2>&1
grep -c "STREAMER: PASS" "$OUT/streamer.log" | grep -q 5   # (3 paces + in-frame uploads + TSan)
echo "-- arriving in the city and walking slowly"
CAM_WALK=0.02 SD_KBPS=3000 SD_SEEK_US=1000 PACE_US=2000 ORDER_EXPECT=1 "$OUT/streamer_asan" "$OUT/city.w3dp" 3000 > "$OUT/walk.log" 2>&1
grep -E "ground:|shown:|progressive|show order" "$OUT/walk.log"
grep -q "STREAMER: PASS" "$OUT/walk.log"
echo "-- running through it (tiles left behind before they finished)"
CAM_WALK=0.3 SD_KBPS=1500 SD_SEEK_US=500 PACE_US=1000 "$OUT/streamer_asan" "$OUT/city.w3dp" 3000 > "$OUT/run.log" 2>&1
grep -E "progressive" "$OUT/run.log"
grep -q "STREAMER: PASS" "$OUT/run.log"
grep -Eq "dropped [1-9]" "$OUT/run.log"
CAM_WALK=0.3 SD_KBPS=1500 PACE_US=3000 "$OUT/streamer_tsan" "$OUT/city.w3dp" 800 2>&1 | grep -E "WARNING: ThreadSanitizer|STREAMER" | sort | uniq -c
echo "STREAMING ORDER: PASS"
# 6.31: the heap reserve (5 x 5 tiles: the far draw distance's ring). Plenty of heap: 5 x 5; 2 MB left: 3 x 3 (the synthetic pack has no COLL)
"$B/w3dconv" --synthetic --tiles 30 46 34 50 -o "$OUT/wide.w3dp" > "$OUT/wide.log" 2>&1
HEAP_EXPECT=full PACE_US=2000 "$OUT/streamer_asan" "$OUT/wide.w3dp" 1500 | grep -E "ring at|HEAP RESERVE|STREAMER"
W3D_HEAP_FREE_MB=2 HEAP_EXPECT=small PACE_US=2000 "$OUT/streamer_asan" "$OUT/wide.w3dp" 1500 | grep -E "ring at|HEAP RESERVE|STREAMER" | tee "$OUT/heap.log"
grep -q "HEAP RESERVE (small): PASS" "$OUT/heap.log"
