#!/bin/bash
# w3dconv --texts-only on synthetic DBCs (stages 5.12 / 5.13b): factions.txt must round-trip the hand-made
# net/tests/data/factions_test.txt, spells.txt must name the trainer's teaching spell after the spell it teaches.
# 7.14: --locale esES takes the Spanish columns of the same DBCs; --locale enUS keeps English.
#   run_text_tests.sh <build dir with w3dconv>
set -e
T=$(cd "$(dirname "$0")" && pwd)
B=${1:-build}
OUT=${OUT:-/tmp/w3d_texts}
rm -rf "$OUT" && mkdir -p "$OUT/data" "$OUT/sd"
python3 "$T/gen_text_dbc.py" "$T/../../net/tests/data/factions_test.txt" "$OUT/data"
"$B/w3dconv" --data "$OUT/data" --texts-only -o "$OUT/sd/test.w3dp"
diff <(grep -v '^#' "$OUT/sd/factions.txt") <(grep -v '^#' "$T/../../net/tests/data/factions_test.txt") && echo "FACTIONS ROUND TRIP: PASS"
printf '78\tHeroic Strike\tRank 1\n284\tHeroic Strike\tRank 2\n6673\tBattle Shout\tRank 1\n1752\tSinister Strike\tRank 1\n1776\tGouge\tRank 1\n3044\tArcane Shot\tRank 1\n75\tAuto Shot\t\n7863\tEnchant Boots - Minor Stamina\t\n2657\tSmelt Copper\t\n7380\tHeroic Strike\tRank 2\n1127\tWell Fed\t\n' > "$OUT/spells.expect"
diff <(grep -v "^@" "$OUT/sd/spells.txt") "$OUT/spells.expect" && echo "SPELL NAMES + TRAINER SPELLS: PASS"
printf '@cd\t100\t15000\t0\t0\t0\n@cd\t439\t0\t4\t120000\t0\n@cd\t1784\t10000\t0\t0\t1\n' > "$OUT/cd.expect"
diff <(grep "^@cd" "$OUT/sd/spells.txt") "$OUT/cd.expect" && echo "SPELL COOLDOWNS: PASS"
printf '@cast\t133\t3500\n' > "$OUT/cast.expect"
diff <(grep "^@cast" "$OUT/sd/spells.txt") "$OUT/cast.expect" && echo "SPELL CAST TIMES: PASS"
# 6.87: auto-attack kinds (1 melee ability starts it, 2 ranged weapon, 4 stops it)
printf '@combat\t1752\t1\n@combat\t1776\t4\n@combat\t3044\t2\n@combat\t75\t2\n@combat\t6660\t2\n' > "$OUT/combat.expect"
diff <(grep "^@combat" "$OUT/sd/spells.txt") "$OUT/combat.expect" && echo "AUTO-ATTACK SPELLS: PASS"
# 6.87: ranges of learnable spells at a unit (Arcane Shot 8-35; the bolts are not learnable here)
[ "$(grep "^@range" "$OUT/sd/spells.txt")" = "$(printf "@range\t3044\t8.0\t35.0")" ] && echo "SPELL RANGES: PASS"
# 6.90: recipes.txt - the enchant names what it fits (armour, feet), the smelting none
grep -q "^R	186	7863	0	0	0	1	0	1	-	10940:1	4:0:256$" "$OUT/sd/recipes.txt" && grep -q "^R	186	2657	0	0	0	0	2840	1	-	2770:1	-1:0:0$" "$OUT/sd/recipes.txt" && echo "RECIPE ITEM REQUIREMENTS: PASS"
# 6.19: skills.txt - the category is kept, the nameless row skipped, tabs / newlines of a description flattened
printf 'S\t43\t6\tSwords\t\nS\t186\t11\tMining\tOre and rock\nS\t98\t10\tLanguage: Common\t\n' > "$OUT/skills.expect"
diff <(grep "^S" "$OUT/sd/skills.txt") "$OUT/skills.expect" && echo "SKILLS: PASS"
# UI stage 10: ground spells (Blizzard by its Targets mask, the bomb by its implicit target; not the nova, not the bolt)
printf '@ground\t10\t30.0\t10.0\n@ground\t4064\t15.0\t5.0\n' > "$OUT/ground.expect"
diff <(grep "^@ground" "$OUT/sd/spells.txt") "$OUT/ground.expect" && echo "GROUND SPELLS: PASS"
# 7.16: an item used on another item (Shiny Bauble on a fishing pole): class, subclass mask, inventory mask, main hand
printf '@item\t8087\t2\t1048576\t0\t1\n@item\t2803\t2\t128\t0\t1\n' > "$OUT/item.expect"
diff <(grep "^@item" "$OUT/sd/spells.txt") "$OUT/item.expect" && echo "ITEM-TARGET SPELLS: PASS"
# 5.48: spells cast without a unit target
printf '@self\t6343\n' > "$OUT/self.expect"
diff <(grep "^@self" "$OUT/sd/spells.txt") "$OUT/self.expect" && echo "SELF-CAST SPELLS: PASS"
# 6.20: spelldesc.bin - descriptions with the numbers in, a buff's tooltip when there is no description, sorted by id
python3 - "$OUT/sd/spelldesc.bin" <<'PY'
import struct, sys
b = open(sys.argv[1], "rb").read()
assert b[:4] == b"SDSC" and struct.unpack_from("<I", b, 4)[0] == 1, "header"
n = struct.unpack_from("<I", b, 8)[0]
got = {}
for k in range(n):
    i, off, ln = struct.unpack_from("<III", b, 12 + k * 12)
    got[i] = b[off:off + ln].decode()
want = {78: "A strike that increases melee damage by 11.", 1127: "Well Fed for 15 sec.",
        6673: "Increases attack power of party members within 10 yards by 15 for 2 min."}
assert got == want, (got, want)
assert list(got) == sorted(got), "not sorted"
print("SPELL DESCRIPTIONS: PASS")
PY
# 7.14: --locale picks the Spanish text column (the DBCs also keep the English one)
mkdir -p "$OUT/data_es" "$OUT/sd_es" "$OUT/sd_en"
python3 "$T/gen_text_dbc.py" "$T/../../net/tests/data/factions_test.txt" "$OUT/data_es" esES > /dev/null
"$B/w3dconv" --data "$OUT/data_es" --locale esES --texts-only -o "$OUT/sd_es/test.w3dp" > /dev/null
"$B/w3dconv" --data "$OUT/data_es" --locale enUS --texts-only -o "$OUT/sd_en/test.w3dp" > /dev/null
grep -q "$(printf '78\tES: Heroic Strike\tES: Rank 1')" "$OUT/sd_es/spells.txt" && echo "LOCALE SPELL NAMES: PASS"
grep -q "$(printf 'S\t43\t6\tES: Swords\t')" "$OUT/sd_es/skills.txt" && echo "LOCALE SKILLS: PASS"
grep -q "ES:" "$OUT/sd_es/factions.txt" && echo "LOCALE FACTIONS: PASS"
grep -q "$(printf '78\tHeroic Strike\tRank 1')" "$OUT/sd_en/spells.txt" && ! grep -q "ES:" "$OUT/sd_en/spells.txt" && echo "LOCALE ENGLISH FALLBACK: PASS"
python3 - "$OUT/sd_es/spelldesc.bin" <<'PY'
import struct, sys
b = open(sys.argv[1], "rb").read()
n = struct.unpack_from("<I", b, 8)[0]
got = {}
for k in range(n):
    i, off, ln = struct.unpack_from("<III", b, 12 + k * 12)
    got[i] = b[off:off + ln].decode()
assert got.get(78, "").startswith("ES: A strike that increases melee damage by 11."), got
print("LOCALE SPELL DESCRIPTIONS: PASS")
PY
