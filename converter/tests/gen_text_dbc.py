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
    recs = b"".join(struct.pack("<" + "".join("f" if isinstance(v, float) else "i" if v < 0 else "I" for v in r), *r) for r in rows)
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
def spell(i, name, rank, effect=0, trigger=0, attr=0, targets=0, rng=0, implA=0, radius=0, rec=0, cat=0, catRec=0, desc='', tip='', base=0, die=0, dur=0):
    r = [0] * 173
    r[0] = i; r[120] = sref(name); r[129] = sref(rank) if rank else 0
    r[61] = effect; r[109] = trigger; r[6] = attr
    r[13] = targets; r[36] = rng; r[82] = implA; r[88] = radius   # UI stage 10: Targets, RangeIndex, ImplicitTargetA, radius
    r[19] = rec; r[2] = cat; r[20] = catRec                        # UI stage 12: RecoveryTime, Category, CategoryRecoveryTime
    r[138] = sref(desc) if desc else 0; r[147] = sref(tip) if tip else 0   # 6.20: Description, ToolTip
    r[76] = base & 0xFFFFFFFF; r[64] = die; r[30] = dur                     # BasePoints, DieSides, DurationIndex
    spells.append(r)
spell(78, "Heroic Strike", "Rank 1", desc="A strike that increases melee damage by $s1.", base=10, die=1)
spell(284, "Heroic Strike", "Rank 2")
spell(6673, "Battle Shout", "Rank 1", desc="Increases attack power of party members within $a1 yards by $s1 for $d.", base=14, die=1, radius=13, dur=21)
spell(7380, "Heroic Strike", "Rank 2", effect=36, trigger=284)   # the trainer's spell teaching rank 2
spell(9999, "Some NPC Spell", "")                               # not learnable: filtered out
spell(1127, "Well Fed", "", effect=6, tip="Well Fed for $d.", dur=5)                           # an aura (food buff): named for the aura bar
spell(9998, "Passive Aura", "", effect=6, attr=0x40)            # passive: not on the aura bar
# UI stage 10: ground spells - Blizzard by its Targets mask, a bomb by its implicit target (28), a self-centred nova
# (range "self") is not one, nor an ordinary bolt
spell(10, "Blizzard", "Rank 1", effect=27, targets=0x40, rng=4, implA=28, radius=13)
spell(4064, "Rough Copper Bomb", "", effect=2, rng=5, implA=16, radius=8)
spell(122, "Frost Nova", "Rank 1", effect=2, rng=1, implA=22, radius=13)
spell(116, "Frostbolt", "Rank 1", effect=2, rng=4, implA=6)
# 5.48: no unit target - Thunder Clap hits the enemies around the caster (implicit 36); the bolt above needs one (6)
spell(6343, "Thunder Clap", "Rank 1", effect=2, implA=36)
# UI stage 12: cooldowns - Charge 15 s, a potion's spell (category 4, 2 min), an "on event" one (Stealth)
spell(100, "Charge", "Rank 1", effect=2, rec=15000)
spell(439, "Healing Potion", "", effect=10, cat=4, catRec=120000)
spell(1784, "Stealth", "Rank 1", effect=2, attr=0x02000000, rec=10000)
open(os.path.join(out, "DBFilesClient", "Spell.dbc"), "wb").write(dbc(173, spells, strings))
open(os.path.join(out, "DBFilesClient", "SpellDuration.dbc"), "wb").write(dbc(4, [[21, 120000, 0, 120000], [5, 15000, 0, 15000]], b"\0"))
open(os.path.join(out, "DBFilesClient", "SpellRange.dbc"), "wb").write(
    dbc(3, [[1, 0.0, 0.0], [4, 0.0, 30.0], [5, 0.0, 15.0]], b"\0"))
open(os.path.join(out, "DBFilesClient", "SpellRadius.dbc"), "wb").write(dbc(4, [[13, 10.0, 0.0, 10.0], [8, 5.0, 0.0, 5.0]], b"\0"))
# 6.19: SkillLine.dbc (ID, Category, Costs, Name[8] + flags, Description[8] + flags, Icon = 22 fields): Swords (weapon), Mining
# (profession) with a description, Common (language), and one row with no name that must be skipped
_sk = b"\0"
def _s(t):
    global _sk
    o = len(_sk); _sk += t.encode() + b"\0"; return o
_rows = []
for _id, _cat, _name, _desc in ((43, 6, "Swords", ""), (186, 11, "Mining", "Ore\tand\nrock"), (98, 10, "Language: Common", ""), (77, 12, "", "")):
    _rows.append([_id, _cat, 0] + [_s(_name) if _name else 0] + [0] * 7 + [0xFF] + [_s(_desc) if _desc else 0] + [0] * 7 + [0xFF, 1])
open(os.path.join(out, "DBFilesClient", "SkillLine.dbc"), "wb").write(dbc(22, _rows, _sk))
open(os.path.join(out, "DBFilesClient", "SkillLineAbility.dbc"), "wb").write(
    dbc(3, [[1, 26, 78], [2, 26, 284], [3, 26, 6673]], b"\0"))
