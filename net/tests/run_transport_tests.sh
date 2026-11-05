#!/bin/bash
# Stage 5.20 / 5.21 net tests on the PC (ASan+UBSan): the taxi-flight spline (spline_test), the ship simulation (transport_test:
# a synthetic ferry, stops, acceleration, speed bound), ObjectTable + Mover with ships and passengers (transport_obj_test).
#   run_transport_tests.sh
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_net_transport}
mkdir -p "$OUT"
for t in spline_test transport_test transport_obj_test; do
    g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$N" "$N/tests/$t.cpp" "$N/objects.cpp" "$N/transport.cpp" \
        "$N/movement.cpp" "$N/inflate.cpp" -o "$OUT/$t"
    "$OUT/$t" | tail -1
done
