#!/bin/bash
# The animation stage: the state machine of creature_view.cpp on hand-made models and the session's animation
# events (unit_state.cpp) after the recorded fight of net/tests/run_stage5.sh.
#   run_anim.sh [fight.w3dl]
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_anim}; mkdir -p "$OUT"
N="$P/../net"
g++ -std=gnu++20 -O1 -g -fsanitize=address,undefined -Wall -I"$P/tests/stub" -I"$P/source" -I"$P/../common" -I"$N" \
    "$P/tests/anim_test.cpp" "$P/source/creature_view.cpp" "$P/source/unit_state.cpp" \
    "$N/game.cpp" "$N/game_social.cpp" "$N/game_spells.cpp" "$N/game_items.cpp" "$N/game_life.cpp" "$N/factions.cpp" \
    "$N/game_factions.cpp" "$N/game_vendor.cpp" "$N/game_quests.cpp" "$N/game_group.cpp" "$N/game_pet.cpp" "$N/game_friends.cpp" \
    "$N/game_services.cpp" "$N/game_mail.cpp" "$N/game_auction.cpp" "$N/game_trade.cpp" "$N/game_water.cpp" "$N/tables.cpp" "$N/result_text.cpp" \
    "$N/objects.cpp" "$N/transport.cpp" "$N/movement.cpp" "$N/client.cpp" "$N/crypto.cpp" "$N/socket.cpp" "$N/inflate.cpp" -o "$OUT/anim_test"
"$OUT/anim_test" "${1:-/tmp/w3d_stage5/fight.w3dl}"
