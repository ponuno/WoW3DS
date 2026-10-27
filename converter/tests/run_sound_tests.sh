#!/bin/bash
# Sound: w3dconv --sound on a synthetic client (gen_sound_data.py), read back by the 3DS-side code (sound_test.cpp).
#   run_sound_tests.sh <w3dconv>
set -e
T=$(cd "$(dirname "$0")" && pwd); W=$(cd "$T/../.." && pwd)
CONV=${1:-$W/../../build/conv/w3dconv}
OUT=${OUT:-/tmp/w3d_sound}
rm -rf "$OUT" && mkdir -p "$OUT"
python3 "$T/gen_sound_data.py" "$OUT/data"
"$CONV" --data "$OUT/data" --sound -o "$OUT/sound" -v
ls "$OUT/sound" "$OUT/sound/music"
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -I"$W/proto3ds/source" -I"$W/common" \
    "$W/proto3ds/tests/sound_test.cpp" "$W/proto3ds/source/audio_core.cpp" -o "$OUT/sound_test"
"$OUT/sound_test" "$OUT/sound"
# the world: recorded sessions of net/tests/run_stage5.sh through SoundWorld
N="$W/net"
L=${LOGS:-/tmp/w3d_stage5}
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -Wall -I"$W/proto3ds/source" -I"$W/common" -I"$N" \
    "$W/proto3ds/tests/sound_world_test.cpp" "$W/proto3ds/source/sound_world.cpp" "$W/proto3ds/source/audio_core.cpp" \
    "$N/game.cpp" "$N/game_social.cpp" "$N/game_spells.cpp" "$N/game_items.cpp" "$N/game_life.cpp" "$N/factions.cpp" \
    "$N/game_factions.cpp" "$N/game_vendor.cpp" "$N/game_quests.cpp" "$N/game_group.cpp" "$N/game_friends.cpp" \
    "$N/game_services.cpp" "$N/game_mail.cpp" "$N/game_auction.cpp" "$N/game_trade.cpp" "$N/tables.cpp" "$N/result_text.cpp" \
    "$N/objects.cpp" "$N/transport.cpp" "$N/movement.cpp" "$N/client.cpp" "$N/crypto.cpp" "$N/socket.cpp" "$N/inflate.cpp" -o "$OUT/sound_world_test"
"$OUT/sound_world_test" "$L/fight.w3dl" "$L/life.w3dl" "$L/trade.w3dl" "$L/party.w3dl"
