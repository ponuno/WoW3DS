#!/bin/bash
# 6.44: merging small static doodads into shared buffers (static_batch.cpp)
set -e
T=$(cd "$(dirname "$0")" && pwd)
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I"$T/../source" -I"$T/../../common" "$T/static_batch_test.cpp" "$T/../source/static_batch.cpp" -o "${TMPDIR:-/tmp}/static_batch_test"
"${TMPDIR:-/tmp}/static_batch_test"
