#!/bin/bash
# Stage 5.45: the zone banner's logic (zone_banner_test.cpp) on the PC with ASan + UBSan.
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_zone}
mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined "$P/tests/zone_banner_test.cpp" "$P/source/zone_banner.cpp" -o "$OUT/zone_banner_test"
"$OUT/zone_banner_test"
