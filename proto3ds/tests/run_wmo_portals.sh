#!/bin/bash
# 6.42: the portal walk that decides which interior groups of a WMO are drawn
set -e
T=$(cd "$(dirname "$0")" && pwd)
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -I"$T/../source" -I"$T/../../common" "$T/wmo_portals_test.cpp" "$T/../source/wmo_portals.cpp" -o "${TMPDIR:-/tmp}/wmo_portals_test"
"${TMPDIR:-/tmp}/wmo_portals_test"
