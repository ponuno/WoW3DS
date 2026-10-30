#!/bin/bash
# Stage 5.36: ground splat of three textures per batch. Usage: run_terrain_tests.sh <build dir with w3dconv>
set -e -o pipefail
B=${1:?build dir}
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_terrain}
rm -rf "$OUT" && mkdir -p "$OUT"
python3 "$T/gen_terrain_layers.py" "$OUT/Data"
"$B/w3dconv" --data "$OUT/Data" --map Test --tiles 32 48 32 48 -o "$OUT/t.w3dp" > "$OUT/conv.log" 2>&1 || { tail -20 "$OUT/conv.log"; exit 1; }
"$B/w3dconv" --info "$OUT/t.w3dp" | grep -q "^OK"
python3 "$T/check_terrain_layers.py" "$OUT/t.w3dp"
# 6.30: cracks between terrain LODs (hills + a patchwork of textures + holes): none with the region LOD of w3d_terrain_lod.h;
# negative control: the old per-batch choice leaves them
python3 "$T/gen_terrain_layers.py" "$OUT/Seams" seams > /dev/null
"$B/w3dconv" --data "$OUT/Seams" --map Test --tiles 32 48 32 48 -o "$OUT/s.w3dp" > "$OUT/conv_s.log" 2>&1 || { tail -20 "$OUT/conv_s.log"; exit 1; }
"$B/w3dconv" --info "$OUT/s.w3dp" | grep -q "^OK"
g++ -std=c++20 -O2 -g -fsanitize=address,undefined -I"$T/../../common" "$T/terrain_seam_test.cpp" -o "$OUT/seam_test"
"$OUT/seam_test" "$OUT/s.w3dp" region
"$OUT/seam_test" "$OUT/s.w3dp" batch expect-cracks
echo "TERRAIN TESTS: PASS"
