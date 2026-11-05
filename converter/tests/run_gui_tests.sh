#!/bin/bash
# The portable part of w3dgui (steps, quoting, settings file); the window itself (gui/w3dgui.cpp) is only compiled by
# the MinGW build, no Wine here.
set -e
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_gui}
mkdir -p "$OUT"
g++ -std=c++20 -Wall -Wextra -fsanitize=address,undefined "$T/gui_core_test.cpp" -o "$OUT/gui_core_test"
"$OUT/gui_core_test"
