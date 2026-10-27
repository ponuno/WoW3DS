#!/bin/bash
# w3dconv --texts-only on synthetic DBCs (stages 5.12 / 5.13b): factions.txt must round-trip the hand-made
# net/tests/data/factions_test.txt, spells.txt must name the trainer's teaching spell after the spell it teaches.
#   run_text_tests.sh <build dir with w3dconv>
set -e
T=$(cd "$(dirname "$0")" && pwd)
B=${1:-build}
OUT=${OUT:-/tmp/w3d_texts}
rm -rf "$OUT" && mkdir -p "$OUT/data" "$OUT/sd"
python3 "$T/gen_text_dbc.py" "$T/../../net/tests/data/factions_test.txt" "$OUT/data"
"$B/w3dconv" --data "$OUT/data" --texts-only -o "$OUT/sd/test.w3dp"
diff <(grep -v '^#' "$OUT/sd/factions.txt") <(grep -v '^#' "$T/../../net/tests/data/factions_test.txt") && echo "FACTIONS ROUND TRIP: PASS"
printf '78\tHeroic Strike\tRank 1\n284\tHeroic Strike\tRank 2\n6673\tBattle Shout\tRank 1\n7380\tHeroic Strike\tRank 2\n1127\tWell Fed\t\n' > "$OUT/spells.expect"
diff "$OUT/sd/spells.txt" "$OUT/spells.expect" && echo "SPELL NAMES + TRAINER SPELLS: PASS"
