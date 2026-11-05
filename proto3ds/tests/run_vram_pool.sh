#!/bin/bash
# 6.43: libctru's VRAM pool (its own mem_pool.cpp) - the re-initialisation fault and the guard of vram_guard.hpp
set -e
T=$(cd "$(dirname "$0")" && pwd)
L=${LIBCTRU:-/home/claude/deps/libctru/libctru}
g++ -std=c++17 -O1 -fpermissive -w -g -I"$L/include" -I"$L/source/allocator" "$T/vram_pool_test.cpp" "$L/source/allocator/mem_pool.cpp" -o "${TMPDIR:-/tmp}/vram_pool_test"
"${TMPDIR:-/tmp}/vram_pool_test"
