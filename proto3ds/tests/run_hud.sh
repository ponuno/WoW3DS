#!/bin/bash
# HUD layout check on the PC (stage 5.9): replays the recorded fight session of net/tests/run_stage5.sh into a
# GameSession, builds the top-screen draw list and rasterises it with the real font atlas (hud_render.py).
#   run_hud.sh [fight.w3dl] [ms]    -> $OUT/hud.png (3x)
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_hud}
LOG=${1:-/tmp/w3d_stage5/fight.w3dl}
MS=${2:-9000}
mkdir -p "$OUT"
N="$P/../net"
D=${DEPS:-/home/claude/deps}
g++ -std=c++20 -O1 -Wall -I"$N" -I"$P/source" -I"$P/tests" -I"$P/tests/ui_stub" -I"$P/../common" -I"$P/../converter/extern/stb" -I"$D/rgetc1" \
    "$P/tests/hud_test.cpp" "$P/source/hud.cpp" "$P/source/hud_game.cpp" "$P/source/ui_style.cpp" "$P/source/ui_assets.cpp" "$P/tests/ui_raster.cpp" \
    "$N/game.cpp" "$N/game_social.cpp" "$N/game_spells.cpp" "$N/game_items.cpp" "$N/game_life.cpp" "$N/factions.cpp" "$N/game_factions.cpp" \
    "$N/game_vendor.cpp" "$N/game_quests.cpp" "$N/game_group.cpp" "$N/game_friends.cpp" "$N/game_services.cpp" "$N/game_mail.cpp" \
    "$N/game_auction.cpp" "$N/game_trade.cpp" "$N/tables.cpp" "$N/result_text.cpp" "$N/objects.cpp" "$N/transport.cpp" "$N/movement.cpp" "$N/client.cpp" \
    "$N/crypto.cpp" "$N/socket.cpp" "$N/inflate.cpp" -x c++ "$D/rgetc1/rg_etc1.cpp" -o "$OUT/hud_test" 2>&1 | grep -v rg_etc1 || true
"$OUT/hud_test" "$LOG" "$MS" "$OUT/hud.png" with-target pad
echo "wrote $OUT/hud.png"
