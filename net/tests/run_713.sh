#!/bin/bash
# 7.13 bug-fix tests on the PC (ASan+UBSan):
#   weaponicon_test  - only Attack / Auto Shot / Shoot / Throw use the weapon icon; ranged abilities keep theirs
#   feed_test        - the Feed Pet list finds Food & Drink (the 1.12 server sends subclass 0), meat and asks for unknown items
#   cannibalize_test - Cannibalize is cast without replacing the corpse with a living soft-target and without a unit target
#   selfmove_test    - a self spline (Charge / taxi) is followed and the mover lands at its exact end
#   run_713.sh
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_net_713}
mkdir -p "$OUT"
SRC="$N/client.cpp $N/crypto.cpp $N/socket.cpp $N/inflate.cpp $N/movement.cpp $N/objects.cpp $N/transport.cpp $N/game.cpp $N/game_social.cpp $N/game_spells.cpp $N/game_items.cpp $N/game_life.cpp $N/result_text.cpp $N/factions.cpp $N/game_factions.cpp $N/game_vendor.cpp $N/game_quests.cpp $N/game_group.cpp $N/game_pet.cpp $N/game_friends.cpp $N/game_services.cpp $N/game_mail.cpp $N/game_auction.cpp $N/game_trade.cpp $N/game_water.cpp $N/tables.cpp"
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
for t in weaponicon_test feed_test cannibalize_test selfmove_test; do
    g++ -std=c++20 $SAN -Wall "$N/tests/$t.cpp" $SRC -o "$OUT/$t"
    "$OUT/$t"
done
