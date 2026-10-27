#!/bin/bash
# Touch-screen UI check on the PC (stage 5.10, UI stages 1-6): game_ui*.cpp against a libctru stub, replaying the
# sessions of net/tests/run_stage5.sh; draws every window with the PC rasteriser to $OUT/ui_*.png (+ ui_sheet.png)
# and taps / long-presses / drags its buttons: every action must send the packet it stands for.
#   run_ui.sh [life.w3dl] [fight.w3dl] [trade.w3dl] [party.w3dl]
# W3D_UI_PACK=ui.w3dp draws with a converted interface pack (else the stub skin).
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_ui}
LOG=${1:-/tmp/w3d_stage5/life.w3dl}
FIGHT=${2:-$(dirname "$LOG")/fight.w3dl}
TRADE=${3:-$(dirname "$LOG")/trade.w3dl}
PARTY=${4:-$(dirname "$LOG")/party.w3dl}
mkdir -p "$OUT"
N="$P/../net"
D=${DEPS:-/home/claude/deps}
NET="$N/game.cpp $N/game_social.cpp $N/game_spells.cpp $N/game_items.cpp $N/game_life.cpp $N/factions.cpp $N/game_factions.cpp \
     $N/game_vendor.cpp $N/game_quests.cpp $N/game_group.cpp $N/game_friends.cpp $N/game_services.cpp $N/game_mail.cpp \
     $N/game_auction.cpp $N/game_trade.cpp $N/game_water.cpp $N/tables.cpp $N/result_text.cpp $N/objects.cpp $N/transport.cpp $N/movement.cpp $N/client.cpp \
     $N/crypto.cpp $N/socket.cpp $N/inflate.cpp"
UI="$P/source/game_ui.cpp $P/source/game_ui_widgets.cpp $P/source/game_ui_tabs.cpp $P/source/game_ui_windows.cpp \
    $P/source/ui_style.cpp $P/source/hud.cpp $P/source/ui_assets.cpp $P/source/pad.cpp $P/source/action_layout.cpp \
    $P/tests/ui_raster.cpp $D/rgetc1/rg_etc1.cpp"
INC="-I$P/tests/ui_stub -I$N -I$P/source -I$P/../common -I$P/tests -I$P/../converter/extern/stb -I$D/rgetc1"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall $INC "$P/tests/ui_test.cpp" $UI $NET -o "$OUT/ui_test"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -I"$P/tests/ui_stub" -I"$N" -I"$P/source" "$P/tests/pad_test.cpp" \
    "$P/source/pad.cpp" -o "$OUT/pad_test"
"$OUT/pad_test"
"$OUT/ui_test" "$LOG" "$FIGHT" "$OUT" "$TRADE" "$P/../net/tests/data/factions_test.txt" "$PARTY"
echo "wrote $OUT/ui_*.png and $OUT/ui_sheet.png"
