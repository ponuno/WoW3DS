#!/bin/bash
# Stage 5.41 net test on the PC (ASan+UBSan): elevators / trams (TransportAnimation), tilted objects, door states, dynamic objects, corpses.
#   run_world_objects.sh
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_net_worldobj}
mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$N" "$N/tests/world_objects_test.cpp" "$N/objects.cpp" "$N/transport.cpp" \
    "$N/movement.cpp" "$N/inflate.cpp" -o "$OUT/world_objects_test"
"$OUT/world_objects_test" | tail -3
