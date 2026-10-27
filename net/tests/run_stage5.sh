#!/bin/bash
# Stage-5 network tests on Linux (no console, no game data needed):
#   1. inflate vs Python zlib is covered by objects_test's fuzzing + the e2e run
#   2. objects_test: positions/fields from a Python-built log, then 3000 rounds of mutated packets (ASan/UBSan)
#   3. end to end: fake_server.py --stage5 (independent Python server side) <-> w3dlogin --walk
set -e
N=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-/tmp/w3d_stage5}
mkdir -p "$OUT"
SRC="$N/client.cpp $N/crypto.cpp $N/socket.cpp $N/inflate.cpp $N/movement.cpp $N/objects.cpp $N/transport.cpp $N/game.cpp $N/game_social.cpp $N/game_spells.cpp $N/game_items.cpp $N/game_life.cpp $N/result_text.cpp $N/factions.cpp $N/game_factions.cpp $N/game_vendor.cpp $N/game_quests.cpp $N/game_group.cpp $N/game_friends.cpp $N/game_services.cpp $N/game_mail.cpp $N/game_auction.cpp $N/game_trade.cpp $N/tables.cpp"
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined"
g++ -std=c++20 $SAN -Wall -Wextra "$N/tools/w3dlogin.cpp" $SRC -o "$OUT/w3dlogin"
g++ -std=c++20 $SAN -Wall "$N/tests/objects_test.cpp" $SRC -o "$OUT/objects_test"

g++ -std=c++20 $SAN "$N/tests/text_test.cpp" -o "$OUT/text_test" && "$OUT/text_test"
python3 "$N/tests/build_objects_log.py" "$OUT/obj.w3dl" "$OUT/obj.expect"
"$OUT/objects_test" "$OUT/obj.w3dl" "$OUT/obj.expect" | tail -1

PORT=${PORT:-4790}
rm -f "$OUT/s5.json"
(cd "$N/tests" && timeout 70 python3 -u fake_server.py --port $PORT --stage5 "$OUT/s5.json" > "$OUT/server.log" 2>&1 &)
sleep 1
echo PASSWORD | timeout 50 "$OUT/w3dlogin" 127.0.0.1:$PORT test --char 0 --stay 19 --walk --log "$OUT/s5.w3dl" --objects 5 --out "$OUT/client.log" > /dev/null
sleep 1
"$OUT/w3dlogin" --replay "$OUT/s5.w3dl" --objects 0 > "$OUT/replay.log"
python3 - "$OUT/s5.json" "$OUT/client.log" <<'PY'
import json, re, sys
r = json.load(open(sys.argv[1])); log = open(sys.argv[2]).read()
e = r["expected"]
m = re.findall(r"objects: (\d+) total \| players (\d+) units (\d+) gameobjects (\d+) items (\d+)", log)[-1]
got = dict(zip(["total", "players", "units", "gameobjects", "items"], map(int, m)))
ok = not r["errors"] and r["active_mover"] == 1 and all(got[k] == e[k] for k in got) and r["speed_ack"] and r["teleport_ack"] \
     and r["root_ack"] and r["unroot_ack"] and r["after_teleport_ok"] and "failed 0" in log \
     and r.get("walk_modes", 0) >= 1 and r.get("run_modes", 0) >= 1 and r.get("walk_moves", 0) >= 1
print("server-side errors:", r["errors"] or "none", "| SET_ACTIVE_MOVER sent", r["active_mover"])
print("objects client/expected:", got, {k: e[k] for k in got})
print("ACKs speed/teleport/root/unroot:", r["speed_ack"], r["teleport_ack"], r["root_ack"], r["unroot_ack"],
      "| max speed seen %.2f" % r["max_speed_seen"], "| move packets", r["move_packets"],
      "| walk/run mode switches", r.get("walk_modes", 0), r.get("run_modes", 0), "walking moves", r.get("walk_moves", 0))
print("STAGE 5 E2E:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY

# Stage 5.6/5.7: chat, target, melee, xp, loot against the same Python server in --fight mode:
# the wolf is 12 yd away (NOTINRANGE first), runs round behind us after two swings (BADFACING -> turn),
# dies (ATTACKSTOP, LOG_XPGAIN), is looted (money + item, release); say / whisper / channel / unknown player.
PORT2=$((PORT + 10))
rm -f "$OUT/fight.json"
(cd "$N/tests" && timeout 60 python3 -u fake_server.py --port $PORT2 --stage5 "$OUT/fight.json" --fight > "$OUT/fight_server.log" 2>&1 &)
sleep 1
printf '9999\tTest Cast\tRank 2\n9998\tTest Interrupt\t\n' > "$OUT/spells.txt"
echo PASSWORD | timeout 50 "$OUT/w3dlogin" 127.0.0.1:$PORT2 test --char 0 --stay 13 --fight Timber --say "hello there" \
    --cast 78 --cast 78 --cast 9999 --cast 9998 --cast 2457 --spells "$OUT/spells.txt" \
    --say "/w Otherguy ping" --say "/1 general hi" --say "/w Nobody x" --say "/bogus" --log "$OUT/fight.w3dl" \
    --objects 0 --out "$OUT/fight_client.log" > /dev/null
sleep 1
"$OUT/w3dlogin" --replay "$OUT/fight.w3dl" --objects 0 > "$OUT/fight_replay.log"
python3 - "$OUT/fight.json" "$OUT/fight_client.log" "$OUT/fight_replay.log" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); log = open(sys.argv[2]).read(); rep = open(sys.argv[3]).read()
want_client = ["FIGHT: Diseased Timber Wolf killed", "chat [npc say] [Marshal McBride] says: Stay alert, recruit.",
               "chat [system] Welcome to the fake realm.", "chat [client] Joined channel 1. General",
               "chat [say] [You] says: hello there", "chat [to] To [Otherguy]: ping",
               "chat [whisper] [Otherguy] whispers: hi ping", "chat [channel] [General] [You]: general hi",
               "chat [client] No player named 'Nobody' is currently playing.", "chat [client] Unknown command /bogus",
               "chat [client] Out of range", "chat [client] You are facing the wrong way", "You miss Diseased Timber Wolf.",
               "You hit Diseased Timber Wolf for 17.", "Diseased Timber Wolf hits you for 3.",
               "Diseased Timber Wolf dies, you gain 45 experience.",
               "chat [client] Loot: 23c item 7073 x1", "xp 45, loot 23 copper + 1 items, attack on 0",
               "chat [client] Your Heroic Strike hits Diseased Timber Wolf for 21.", "chat [client] Heroic Strike is not ready (",
               "chat [client] Test Interrupt: interrupted", "spell names: loaded (5)",
               "SPELLS: book 6, bar 5, casts that went off 3, last failure code 35"]
missing = [w for w in want_client if w not in log]
chat_in = [c[:3] + [c[3]] for c in r["chat_in"]]
ok = (not r["errors"] and not missing and r["wolf_dead"] and r["swings"] >= 4 and "range" in r["attack_errors"]
      and "facing" in r["attack_errors"] and r["loot"] == ["open", "money", "item0", "release"] and r["lang"] == 7
      and [c[0] for c in chat_in] == [0, 6, 14, 6] and r["selection"] and "Diseased Timber Wolf dies" in rep
      and r["casts"] == [[78, 2], [9999, 2], [9998, 2], [2457, 2]])
print("server-side errors:", r["errors"] or "none")
print("swings", r["swings"], "| attack errors", r["attack_errors"], "| loot", r["loot"], "| chat from client", chat_in)
print("casts seen by the server:", r["casts"])
print("client lines missing:", missing or "none")
print("STAGE 5.6/5.7 FIGHT+CHAT E2E:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY

g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined "$N/tests/social_fuzz.cpp" $SRC -o "$OUT/social_fuzz"
"$OUT/social_fuzz" "$OUT/fight.w3dl" 2000

# Stage 5.10: bags (names from item queries), loot window (one item, then take all), equip / use / failed equip /
# destroy / unequip, gossip with NPC text, then three deaths: corpse run (ghost speed, water walking ACKs, reclaim
# delay and radius), spirit healer (gossip -> confirm -> activate), a player's resurrection.
PORT3=$((PORT + 20))
rm -f "$OUT/life.json"
(cd "$N/tests" && timeout 70 python3 -u fake_server.py --port $PORT3 --stage5 "$OUT/life.json" --life > "$OUT/life_server.log" 2>&1 &)
sleep 1
echo PASSWORD | timeout 60 "$OUT/w3dlogin" 127.0.0.1:$PORT3 test --char 0 --stay 22 --life --log "$OUT/life.w3dl" \
    --objects 0 --out "$OUT/life_client.log" > /dev/null
sleep 1
"$OUT/w3dlogin" --replay "$OUT/life.w3dl" --objects 0 > "$OUT/life_replay.log"
python3 - "$OUT/life.json" "$OUT/life_client.log" "$OUT/life_replay.log" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); log = open(sys.argv[2]).read(); rep = open(sys.argv[3]).read(); L = r["life"]
want = ["INVENTORY: 12s 34c money, 18 free bag slots", "Main Hand 15   Worn Shortsword        x1 (Common, One-Hand) 20/20",
        "bag 19 slot 0  Linen Cloth            x2 (Common)", "Backpack 23    Tough Hunk of Bread    x4 (Common, use)",
        "LOOT WINDOW: 50c | [0] Linen Cloth x1 | [2] Tough Wolf Meat x1 | [3] Item 99999 x1 (unknown item)",
        "INVENTORY AFTER: 12s 84c money, 16 free bag slots", "Chest 4        Brawler's Harness      x1 (Common, Chest) 25/25",
        "Backpack 23    Tough Hunk of Bread    x3 (Common, use)", "Backpack 25    Recruit's Shirt        x1 (Common, Shirt)",
        "Backpack 28    Item 99999             x1 (unknown item)",
        'GOSSIP: "Greetings, Testone. Stay alert. The kobolds are near." | option 0: Tell me about Northshire. | option 1: Train me. | quest 783: A Threat Within (level 1)',
        "chat [loot] You receive loot: [Tough Wolf Meat].", "chat [client] You loot 50c.",
        "chat [client] This item cannot be equipped", "LIFE: dead (A says: You are dead (release your spirit first))",
        "LIFE: ghost at the graveyard, corpse 50.0 yd away, reclaim in", "water walking 1",
        "interact as ghost: Your corpse is 50 yards away", "LIFE step 14 done: reclaim the corpse",
        'SPIRIT HEALER: 1 option(s), first "Return me to life."', "chat [client] Your equipment has lost durability.",
        "LIFE: Otherguy offers a resurrection",
        "LIFE: deaths 3, resurrections 3, gossip menus 2, item queries 9/9, flag ACKs 4, last inventory error 20 (This item cannot be equipped)"]
missing = [w for w in want if w not in log]
ok = (not r["errors"] and not missing and "TIMED OUT" not in log
      and L["loot"] == ["open", "item2", "money", "item0", "item3", "release"]
      and L["equip"] == [[255, 25], [255, 24]] and L["use"] == [[255, 23, 0, 0]] and L["destroy"] == [[19, 0, 2]]
      and L["unequip"] == [[255, 3, 255]] and L["npc_text"] == 1 and L["repops"] == 2 and L["reclaims"] == 1
      and L["revived"] == ["corpse", "healer", "resurrect"] and L["flag_acks"] == [[720, 1], [720, 0], [720, 1], [720, 0]]
      and L["ghost_moves"] > 0 and L["ghost_moves_without_flag"] == 0 and L["alive_moves_with_flag"] == 0
      and sorted(L["item_queries"]) == sorted(set(L["item_queries"])) and "failed 0" in rep)
print("server-side errors:", r["errors"] or "none")
print("loot", L["loot"], "| equip", L["equip"], "use", L["use"], "destroy", L["destroy"], "unequip", L["unequip"])
print("deaths", L["deaths"], "revived", L["revived"], "| flag ACKs", L["flag_acks"], "| ghost moves", L["ghost_moves"],
      "without water walking", L["ghost_moves_without_flag"], "| item queries", len(L["item_queries"]))
print("client lines missing:", missing or "none")
print("STAGE 5.10 ITEMS+GOSSIP+DEATH E2E:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
"$OUT/social_fuzz" "$OUT/life.w3dl" 3000 --no-cycle

# Stages 5.12-5.14: faction reactions (hand-made factions.txt + the server's reputation list), quests (marks, gossip ->
# details -> accept, kill / item progress, complete -> items wanted -> reward choice, a quest list, decline, abandon) and a
# vendor (gossip option -> list, buy bundles, stock running out, too expensive, sell, buyback, refused sale, repair,
# the window closing when we walk away), a trainer (list, refusals, learning, the list again) and an innkeeper.
PORT4=$((PORT + 30))
rm -f "$OUT/trade.json"
(cd "$N/tests" && timeout 70 python3 -u fake_server.py --port $PORT4 --stage5 "$OUT/trade.json" --trade > "$OUT/trade_server.log" 2>&1 &)
sleep 1
printf '7380\tHeroic Strike\tRank 2\n284\tHeroic Strike\tRank 2\n3127\tParry\t\n2457\tBattle Stance\t\n1127\tWell Fed\t\n6788\tWeakened Soul\t\n772\tRend\tRank 1\n' > "$OUT/trade_spells.txt"
echo PASSWORD | timeout 60 "$OUT/w3dlogin" 127.0.0.1:$PORT4 test --char 0 --stay 30 --trade --factions "$N/tests/data/factions_test.txt" \
    --spells "$OUT/trade_spells.txt" \
    --log "$OUT/trade.w3dl" --objects 0 --out "$OUT/trade_client.log" > /dev/null
sleep 1
"$OUT/w3dlogin" --replay "$OUT/trade.w3dl" --objects 0 > "$OUT/trade_replay.log"
python3 - "$OUT/trade.json" "$OUT/trade_client.log" "$OUT/trade_replay.log" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); log = open(sys.argv[2]).read(); rep = open(sys.argv[3]).read(); T = r["trade"]
want = ["REACTIONS: Booty Bay Bruiser=Neutral/no Brother Danil=Friendly/no Defias Bandit=Hated/attack Deputy Willem=Friendly/no "
        "Hordeguy=Hostile/no Invisible Stalker=Hostile/no Kobold Vermin=Hostile/attack Marshal McBride=Friendly/no "
        "Pvpguy=Hostile/attack Rabbit=Neutral/attack Stormwind City Guard=Friendly/no Young Wolf=Hostile/attack",
        "chat [client] You cannot attack that target", "MARKS: Willem ! McBride !",
        "AURAS: Well Fed slot 0 cancelable 1800s | Weakened Soul slot 32 debuff 0s | wolf: Rend x3 | wolf targets us",
        "cancel debuff sent 0, buff sent 1", "chat [client] You can't cancel that.", "AURAS NOW: 1 (Weakened Soul)",
        'WILLEM GOSSIP: 1 quest(s), first 7 "Kobold Camp Cleanup" icon 5',
        'DETAILS: 7 "Kobold Camp Cleanup" | Your first task, Testone, is clearing out the kobolds.|Be careful. | Kill 2 Kobold Vermin '
        'and bring a piece of Tough Wolf Meat to Deputy Willem. | choices: Draped Cloak Kobold Hunter Gloves | money 35c',
        "chat [client] Quest accepted: Kobold Camp Cleanup", 'LOG: slot 0 quest 7 "Kobold Camp Cleanup"',
        "chat [client] Kobold Vermin slain: 1/2", "chat [client] Kobold Vermin slain: 2/2", "chat [client] Tough Wolf Meat: 1/1",
        "chat [client] Kobold Camp Cleanup (Complete)",
        "PROGRESS: Kobold Vermin slain: 2/2 | Tough Wolf Meat: 1/1 | (complete) | Willem now ?", "WILLEM GOSSIP 2: quest 7 icon 4",
        'REQUEST ITEMS: "Do you have the meat, Testone?" wants Tough Wolf Meat x1, completable 1',
        'OFFER REWARD: "Well done, Testone. Take one of these." 2 choices, money 35c',
        "chat [client] Kobold Camp Cleanup completed. 120 xp. 35c.", "chat [client] Reputation with Stormwind increased by 250.",
        "chat [client] You are now Friendly with Booty Bay.",
        "REPUTATION: Booty Bay Friendly 3500 | Defias Brotherhood Hated -42000 at war | Stormwind Friendly 3250 | Bruiser now Friendly/no",
        'MCBRIDE LIST: "Greetings, Testone. Northshire needs you." | 33 Wolves Across the Border (5) | 783 A Threat Within (5) |',
        "DECLINE: A Threat Within", "ABANDON: slot 0 quest 33", 'DANIL GOSSIP: option 0 "Let me browse your goods." icon 1',
        "VENDOR: repair 1 | 1 Refreshing Spring Water 25c x5 stock - | 2 Small Throwing Knife 15c x1 stock 2 | "
        "3 Gilded Ceremonial Blade 100g 0s 0c x1 stock - | money 15s 35c",
        "VENDOR BUY: water x5, knives sold out (third buy sent 0), blade too dear (sent 0), money 14s 80c",
        "chat [client] Bought Refreshing Spring Water x5 for 25c.", "chat [client] Sold Linen Cloth x2 for 26c.",
        "sell hearthstone sent 0", "chat [client] The merchant doesn't want that item.", "BUYBACK: 1 item(s), first Linen Cloth x2 for 26c",
        "chat [client] Repaired for 15c.", "REPAIRED: sword 20/20, money 14s 65c",
        'TRAINER: "Hello, warrior! Ready for some training?" | Heroic Strike (Rank 2) state 0 L4 10c | Parry () state 1 L6 1s 0c | '
        'Battle Stance () state 2 L1 0c |', "train parry sent 0, known sent 0", "chat [client] You must be level 6 to learn that.",
        "chat [client] You already know that.", "TRAINED: knows 284 1, list again with state 2, money 14s 55c",
        "chat [client] You have learned a new spell: Heroic Strike (Rank 2)", "chat [client] This inn is now your home.",
        'OBJECT: Milly\'s Harvest verb Loot, interact "", target unchanged 1', "CHEST LOOT: Milly's Harvest x1",
        "CLAM: openable 1, usable 0", "CLAM LOOT: Clam Meat x1", "POSTER: mark !, verb Quest", 'POSTER LIST: "WANTED: Hogger" | Wanted: Hogger',
        "TRADE: vendor open 0 after walking away | bought 3, sold 1, buybacks 1, repairs 1, quests accepted 2, completed 1",
        "trained 1, money 14s 55c"]
missing = [w for w in want if w not in log]
ok = (not r["errors"] and not missing and "TIMED OUT" not in log and T["attack_swings"] == 0
      and T["quest_flow"] == [["query", 7], ["accept", 7], ["complete", 7], ["reward?", 7], ["choose", 7, 1], ["query", 783],
                              ["query", 33], ["accept", 33]]
      and T["status_answers"][:3] == [5, 3, 4] and T["hello"] == 2 and T["cancels"] == 2 and T["removed"] == [[0, 33]]
      and T["buys"] == [[159, 1], [2947, 1], [2947, 1]] and T["sells"] == [[2589, 0], [38, 0]] and T["buybacks"] == [69]
      and T["repairs"] == [0] and T["money_end"] == 1455 and sorted(T["quest_queries"]) == [7, 33] and "failed 0" in rep
      and T["trained"] == [7380] and T["trainer_lists"] == 2 and T["binder"] == 1 and T["aura_cancels"] == [1127]
      and T["go_use"] == ["chest", "poster"] and T["go_loot"] == [0, "release", 0, "release clam"] and T["opened"] == [[255, 26]])
print("server-side errors:", r["errors"] or "none")
print("quest flow", T["quest_flow"], "| Willem marks", T["status_answers"], "| status queries", T["status_queries"])
print("buys", T["buys"], "sells", T["sells"], "buybacks", T["buybacks"], "repairs", T["repairs"], "money at the end", T["money_end"])
print("trainer lists", T["trainer_lists"], "trained", T["trained"], "| inn bound", T["binder"], "| objects used", T["go_use"], T["go_loot"])
print("client lines missing:", missing or "none")
print("STAGE 5.12-5.14 FACTIONS+QUESTS+VENDOR+TRAINER E2E:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
"$OUT/social_fuzz" "$OUT/trade.w3dl" 3000 --no-cycle --factions "$N/tests/data/factions_test.txt"

# Stage 5.17: a party - invitation (accept), the member list (one member in range, one far away: party member stats),
# a partial stats update, becoming the leader, /uninvite, a failed /invite, /leaveparty, a second invitation (decline).
PORT5=$((PORT + 40))
rm -f "$OUT/party.json"
(cd "$N/tests" && timeout 40 python3 -u fake_server.py --port $PORT5 --stage5 "$OUT/party.json" --party > "$OUT/party_server.log" 2>&1 &)
sleep 1
echo PASSWORD | timeout 30 "$OUT/w3dlogin" 127.0.0.1:$PORT5 test --char 0 --stay 9 --party --log "$OUT/party.w3dl" --objects 0 \
    --out "$OUT/party_client.log" > /dev/null
sleep 1
python3 - "$OUT/party.json" "$OUT/party_client.log" <<'PY'
import json, sys
r = json.load(open(sys.argv[1])); log = open(sys.argv[2]).read(); P = r["party"]
want = ["INVITE from Otherguy", "chat [client] You joined a party.",
        "PARTY: leader Otherguy | Otherguy L11 250/260 online 1 (in range) | Farguy L20 280/300 zone 12 online 1 | leader us 0",
        "FARGUY HURT: 150/300", "LEADER: us", "chat [client] Testone is now the group leader.",
        "chat [client] Farguy leaves the party.", "chat [client] Cannot find 'Nobody'.", "chat [client] You leave the group.",
        "SECOND INVITE from Pestguy while not in a group: declined", "PARTY DONE: lists 4, stats 3"]
missing = [w for w in want if w not in log]
ok = (not r["errors"] and not missing and "TIMED OUT" not in log and P["accepts"] == 1 and P["declines"] == 1
      and P["invites"] == ["Nobody"] and P["uninvites"] == ["Farguy"] and P["disbands"] == 1 and sorted(P["stat_requests"]) == [0x99, 0x123])
print("server-side errors:", r["errors"] or "none")
print("party:", P)
print("client lines missing:", missing or "none")
print("STAGE 5.17 PARTY E2E:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
PY
"$OUT/social_fuzz" "$OUT/party.w3dl" 3000 --no-cycle

# Regression: a real 60 s session recorded at Northshire Abbey on play.stonetavern.app (stage 5a).
"$OUT/w3dlogin" --replay "$N/tests/data/stonetavern_abbey.w3dl" --objects 0 | tee "$OUT/real.log" | head -3
grep -q "failed 0" "$OUT/real.log" && grep -q "objects: 87 total" "$OUT/real.log" && echo "REAL SESSION REPLAY: PASS"
# The same real session also pins the spell layer (stage 5.8): 40 spells in the book, Attack + Heroic Strike on the
# server's action bar (slots 72/73 = the warrior stance bar), the item in slot 83 is not a spell. Since stage 5.10 the
# replay knows our GUID (the SELF object), so the one SPELL_GO of ours in the log (a login spell) counts: 1 cast.
grep -q "spells: 40 in the book, 1 casts of ours, action bar: Attack(6603) Heroic Strike(78)" "$OUT/real.log" && echo "REAL SPELL BOOK + ACTION BAR: PASS"
# Stage 5.10: the same real session pins the inventory layout (PLAYER_FIELD_INV_SLOT_HEAD 486, item objects with
# stack counts) and the look fields at PLAYER_BYTES 193 / _2 194 (not WoWee's 191/192, which are the guild fields).
grep -q "inventory: 7 items, 0c money: 255/3=38 255/6=39 255/7=40 255/15=25 255/16=2362 255/23=117x4 255/24=6948" "$OUT/real.log" && \
grep -q "look: skin 1 face 6 hair 8 colour 3 facial 2 | life alive" "$OUT/real.log" && echo "REAL INVENTORY + LOOK FIELDS: PASS"
