"""Synthetic DBCs (1.12 layouts) for w3dconv --texts-only: FactionTemplate.dbc + Faction.dbc from a factions.txt
(the round trip -> factions.txt, stage 5.12), and a small Spell.dbc + SkillLineAbility.dbc with a trainer's teaching
spell (spells.txt names the teaching spell after the spell it teaches, stage 5.13b).
  gen_text_dbc.py factions.txt outdir [locale]
7.14: locale "esES" also fills the Spanish text columns (the English ones stay); run w3dconv with --locale esES."""
import struct, sys, os

src, out = sys.argv[1], sys.argv[2]
locale = sys.argv[3] if len(sys.argv) > 3 else "enUS"
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
    row = vals + [0] + [off] + [0] * 7 + [0] + [0] * 8 + [0]
    if locale == "esES" and name:   # 7.14: the 7th text column (enUS, koKR, frFR, deDE, zhCN, zhTW, esES, esMX)
        es = b"ES: " + name + b"\0"
        row[25] = len(strings)
        strings += es
    rows.append(row)
open(os.path.join(out, "DBFilesClient", "Faction.dbc"), "wb").write(dbc(37, rows, strings))
print(f"{len(T)} templates, {len(F)} factions")

# Spell.dbc: 173 fields, name 120, rank 129, Effect 61-63, EffectTriggerSpell 109-111
strings = b"\0"
def sref(t):
    global strings
    off = len(strings); strings += t.encode() + b"\0"; return off
spells = []
spell_text = {}   # 7.14: English texts to copy into the esES columns of the synthetic DBC
def spell(i, name, rank, effect=0, trigger=0, attr=0, targets=0, rng=0, implA=0, radius=0, rec=0, cat=0, catRec=0, desc='', tip='', base=0, die=0, dur=0, cast=0, dmg=0, attr2=0, eqClass=-1, eqSub=0, eqInv=0, itemType=0, reagent=0):
    r = [0] * 173
    r[0] = i; r[120] = sref(name); r[129] = sref(rank) if rank else 0
    spell_text[i] = (name, rank, desc, tip)
    r[61] = effect; r[109] = trigger; r[6] = attr
    r[18] = cast                                                   # 6.64: CastingTimeIndex
    r[164] = dmg; r[8] = attr2                                     # 6.87: DmgClass, AttributesEx2
    r[58] = eqClass & 0xFFFFFFFF; r[59] = eqSub; r[60] = eqInv; r[103] = itemType; r[42] = reagent; r[50] = 1 if reagent else 0   # 6.90
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
spell(133, "Fireball", "Rank 1", effect=2, rng=4, implA=6, cast=4, dmg=1)     # 6.64: 3.5 s cast
spell(2060, "Instant Thing", "", effect=2, rng=4, implA=6, cast=1)     # SpellCastTimes row 1 = 0 ms: no @cast line
# 6.87: auto-attack - a melee ability at the enemy (starts it), one that stops it (Gouge), a hunter's shot and Auto Shot
# (ranged), an NPC's Shoot (ranged, not learnable), Thunder Clap (melee class but around the caster: does not start it)
spell(1752, "Sinister Strike", "Rank 1", effect=2, implA=6, dmg=2)
spell(1776, "Gouge", "Rank 1", effect=2, implA=6, dmg=2, attr=0x100000)
spell(3044, "Arcane Shot", "Rank 1", effect=2, implA=6, dmg=3, attr=0x2, rng=35)
spell(75, "Auto Shot", "", effect=2, implA=6, dmg=3, attr=0x2, attr2=0x20)
spell(6660, "Shoot", "", effect=2, implA=6, dmg=3, attr=0x2)
spells[[r[0] for r in spells].index(6343)][164] = 2
# 6.90: recipes - an enchant for boots (armour class 4, inventory type 8 = feet) and a plain item (no requirement)
spell(7863, "Enchant Boots - Minor Stamina", "", effect=53, eqClass=4, eqInv=1 << 8, reagent=10940)
spell(2657, "Smelt Copper", "", effect=24, itemType=2840, reagent=2770)
# 7.16: an item used on another item - Shiny Bauble's spell: Targets 0x10 (an item), only a fishing pole (weapon class 2,
# subclass 20) and SPELL_ATTR_HELD_ITEM_ONLY (0x200: the original client takes the item from the main hand)
spell(8087, "Shiny Bauble", "", effect=6, targets=0x10, attr=0x200, eqClass=2, eqSub=1 << 20)
# ... and the older shape: Targets left 0, the main-hand attribute and the item class are what identify it (a weapon oil)
spell(2803, "Weapon Oil", "", effect=54, attr=0x200, eqClass=2, eqSub=1 << 7)
# Fishing also names the main hand, but it summons a bobber (not an enchant): it must NOT become an item-on-item spell
spell(7620, "Fishing", "", effect=76, attr=0x200, eqClass=2, eqSub=1 << 20)
if locale == "esES":   # 7.14: name 120+6, rank 129+6, description 138+6, tooltip 147+6
    for r in spells:
        name, rank, desc, tip = spell_text.get(r[0], ("", "", "", ""))
        if name: r[126] = sref("ES: " + name)
        if rank: r[135] = sref("ES: " + rank)
        if desc: r[144] = sref("ES: " + desc)
        if tip: r[153] = sref("ES: " + tip)
open(os.path.join(out, "DBFilesClient", "SpellCastTimes.dbc"), "wb").write(dbc(4, [[1, 0, 0, 0], [4, 3500, 0, 0]], b"\0"))
open(os.path.join(out, "DBFilesClient", "Spell.dbc"), "wb").write(dbc(173, spells, strings))
open(os.path.join(out, "DBFilesClient", "SpellDuration.dbc"), "wb").write(dbc(4, [[21, 120000, 0, 120000], [5, 15000, 0, 15000]], b"\0"))
open(os.path.join(out, "DBFilesClient", "SpellRange.dbc"), "wb").write(
    dbc(3, [[1, 0.0, 0.0], [4, 0.0, 30.0], [5, 0.0, 15.0], [35, 8.0, 35.0]], b"\0"))
open(os.path.join(out, "DBFilesClient", "SpellRadius.dbc"), "wb").write(dbc(4, [[13, 10.0, 0.0, 10.0], [8, 5.0, 0.0, 5.0]], b"\0"))
# 6.19: SkillLine.dbc (ID, Category, Costs, Name[8] + flags, Description[8] + flags, Icon = 22 fields): Swords (weapon), Mining
# (profession) with a description, Common (language), and one row with no name that must be skipped
_sk = b"\0"
def _s(t):
    global _sk
    o = len(_sk); _sk += t.encode() + b"\0"; return o
_rows = []
for _id, _cat, _name, _desc in ((43, 6, "Swords", ""), (186, 11, "Mining", "Ore\tand\nrock"), (98, 10, "Language: Common", ""), (77, 12, "", "")):
    _row = [_id, _cat, 0] + [_s(_name) if _name else 0] + [0] * 7 + [0xFF] + [_s(_desc) if _desc else 0] + [0] * 7 + [0xFF, 1]
    if locale == "esES":   # DisplayName 3+6, Description 12+6
        if _name: _row[9] = _s("ES: " + _name)
        if _desc: _row[18] = _s("ES: " + _desc)
    _rows.append(_row)
open(os.path.join(out, "DBFilesClient", "SkillLine.dbc"), "wb").write(dbc(22, _rows, _sk))
# SkillLineAbility: 12 fields (1.12; recipes.txt reads MinSkillLineRank 7, trivial high 10 / low 11)
open(os.path.join(out, "DBFilesClient", "SkillLineAbility.dbc"), "wb").write(
    dbc(12, [r + [0] * 9 for r in [[1, 26, 78], [2, 26, 284], [3, 26, 6673], [4, 38, 1752], [5, 38, 1776], [6, 51, 3044], [7, 51, 75], [8, 186, 7863], [9, 186, 2657]]], b"\0"))
