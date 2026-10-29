#!/bin/bash
# UI stage 8: voices (voice.cpp) over the recorded sessions of net/tests/run_stage5.sh + a scripted NPC / errors / emote.
#   run_voice.sh   (LOGS=/tmp/w3d_stage5)
set -e
P=$(cd "$(dirname "$0")/.." && pwd); N="$P/../net"
OUT=${OUT:-/tmp/w3d_voice}; L=${LOGS:-/tmp/w3d_stage5}
mkdir -p "$OUT"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -I"$P/source" -I"$P/../common" -I"$N" \
    "$P/tests/voice_test.cpp" "$P/source/voice.cpp" "$P/source/audio_core.cpp" \
    "$N/game.cpp" "$N/game_social.cpp" "$N/game_spells.cpp" "$N/game_items.cpp" "$N/game_life.cpp" "$N/factions.cpp" \
    "$N/game_factions.cpp" "$N/game_vendor.cpp" "$N/game_quests.cpp" "$N/game_group.cpp" "$N/game_pet.cpp" "$N/game_friends.cpp" \
    "$N/game_services.cpp" "$N/game_mail.cpp" "$N/game_auction.cpp" "$N/game_trade.cpp" "$N/game_water.cpp" "$N/tables.cpp" "$N/result_text.cpp" \
    "$N/objects.cpp" "$N/transport.cpp" "$N/movement.cpp" "$N/client.cpp" "$N/crypto.cpp" "$N/socket.cpp" "$N/inflate.cpp" -o "$OUT/voice_test"
"$OUT/voice_test" "$L/fight.w3dl" "$L/life.w3dl" "$L/trade.w3dl" "$L/party.w3dl"
