"""Synthetic DBCs (1.12 layouts) for w3dconv --texts-only: FactionTemplate.dbc + Faction.dbc from a factions.txt
(the round trip -> factions.txt, stage 5.12), and a small Spell.dbc + SkillLineAbility.dbc with a trainer's teaching
spell (spells.txt names the teaching spell after the spell it teaches, stage 5.13b).
  gen_text_dbc.py factions.txt outdir"""
import struct, sys, os

src, out = sys.argv[1], sys.argv[2]
T, F = [], []
for line in open(src):
    c = line.rstrip("\n").split("\t")
    if c[0] == "T": T.append([int(x) for x in c[1:15]])
    elif c[0] == "F": F.append(c[1:])

def dbc(fields, rows, strings):
    recs = b"".join(struct.pack("<" + "".join("i" if isinstance(v, int) and v < 0 else "I" for v in r), *r) for r in rows)
    return b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, len(strings)) + recs + strings

os.makedirs(os.path.join(out, "DBFilesClient"), exist_ok=True)
open(os.path.join(out, "DBFilesClient", "FactionTemplate.dbc"), "wb").write(dbc(14, T, b"\0"))
strings = b"\0"
rows = []
for f in F:
    vals = [int(x) for x in f[:18]]
    name = f[18].encode() if len(f) > 18 else b""
    off = len(strings)
    strings += name + b"\0"
    # 37 fields: 0-17 as in factions.txt, 18 parent, 19-26 names (enUS first), 27 flags, 28-35 descriptions, 36 flags
    rows.append(vals + [0] + [off] + [0] * 7 + [0] + [0] * 8 + [0])
open(os.path.join(out, "DBFilesClient", "Faction.dbc"), "wb").write(dbc(37, rows, strings))
print(f"{len(T)} templates, {len(F)} factions")

# Spell.dbc: 173 fields, name 120, rank 129, Effect 61-63, EffectTriggerSpell 109-111
strings = b"\0"
def sref(t):
    global strings
    off = len(strings); strings += t.encode() + b"\0"; return off
spells = []
def spell(i, name, rank, effect=0, trigger=0, attr=0):
    r = [0] * 173
    r[0] = i; r[120] = sref(name); r[129] = sref(rank) if rank else 0
    r[61] = effect; r[109] = trigger; r[6] = attr
    spells.append(r)
spell(78, "Heroic Strike", "Rank 1")
spell(284, "Heroic Strike", "Rank 2")
spell(6673, "Battle Shout", "Rank 1")
spell(7380, "Heroic Strike", "Rank 2", effect=36, trigger=284)   # the trainer's spell teaching rank 2
spell(9999, "Some NPC Spell", "")                               # not learnable: filtered out
spell(1127, "Well Fed", "", effect=6)                           # an aura (food buff): named for the aura bar
spell(9998, "Passive Aura", "", effect=6, attr=0x40)            # passive: not on the aura bar
open(os.path.join(out, "DBFilesClient", "Spell.dbc"), "wb").write(dbc(173, spells, strings))
open(os.path.join(out, "DBFilesClient", "SkillLineAbility.dbc"), "wb").write(
    dbc(3, [[1, 26, 78], [2, 26, 284], [3, 26, 6673]], b"\0"))
