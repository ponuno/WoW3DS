"""Synthetic client data for w3dconv --spell-fx (stage 5.33): Spell / SpellVisual / SpellVisualKit / SpellVisualEffectName
DBCs and the effect models they name.

    python3 gen_spellfx_data.py <Data dir> <expect.json>

Spells: 78 "Heroic Strike" (no visual: finds the name column), twelve Fireball ranks (visual 10: precast fire in both
hands, cast at the feet, a flying fireball, impact on the chest; missile speed 20 in column 34), Frostbolt (visual 11:
a missile only, speed 28), twelve heals (visual 12: precast on the chest, impact at the feet; speed 0) and Smite (visual
13: its effect model is missing from the client - counted as failed, the visual dropped).
"""
import json, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m2_writer import m2, blp

out, expect_path = sys.argv[1], sys.argv[2]

def path(p):
    full = os.path.join(out, *p.replace("\\", "/").split("/"))
    os.makedirs(os.path.dirname(full), exist_ok=True)
    return full

def dbc(name, fields, rows):
    """rows: lists of int / float / str (strings go to the string table)"""
    strs = bytearray(b"\0"); offs = {}
    def s(t):
        if t not in offs: offs[t] = len(strs); strs.extend(t.encode() + b"\0")
        return offs[t]
    recs = b""
    for r in rows:
        r = list(r) + [0] * (fields - len(r))
        for v in r:
            if isinstance(v, str): recs += struct.pack("<I", s(v))
            elif isinstance(v, float): recs += struct.pack("<f", v)
            else: recs += struct.pack("<I", v)
    open(path("DBFilesClient\\" + name), "wb").write(
        b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, len(strs)) + recs + bytes(strs))

# ---- Spell.dbc: 173 fields, name at 120 (English), visual at 115, missile speed at 34
NAME, VIS, SPEED = 120, 115, 34
def spell(sid, name, vis, speed):
    r = [0] * 173
    r[0] = sid; r[NAME] = name; r[VIS] = vis; r[SPEED] = float(speed)
    r[20] = (sid * 7919) % 1000   # a busy integer column that must not pass for the speed
    return r
rows = [spell(78, "Heroic Strike", 0, 0)]
fireballs = [133, 143, 145, 3140, 8400, 8401, 8402, 10148, 10149, 10150, 10151, 25306]
heals = [2050, 2052, 2053, 2054, 2055, 6063, 6064, 2060, 10963, 10964, 10965, 25314]
rows += [spell(s, "Fireball", 10, 20) for s in fireballs]
rows += [spell(116, "Frostbolt", 11, 28)]
rows += [spell(s, "Heal", 12, 0) for s in heals]
rows += [spell(585, "Smite", 13, 0)]
rows += [spell(17, "Power Word: Shield", 14, 0), spell(689, "Drain Life", 15, 0)]   # stage 5.34: an aura, a channel
dbc("Spell.dbc", 173, rows)

# ---- SpellVisual: id, precast kit, cast kit, impact kit, state, statedone, channel, has missile, missile model, ...
dbc("SpellVisual.dbc", 16, [
    [10, 100, 101, 102, 0, 0, 0, 1, 1003],
    [11, 0, 0, 0, 0, 0, 0, 1, 1007],
    [12, 103, 0, 104],
    [13, 0, 0, 105],
    [14, 0, 0, 0, 106],               # state kit (column 4): the shield around the unit while the aura lasts
    [15, 0, 0, 0, 0, 0, 107],         # channel kit (column 6): in the hands while channelling
])
# ---- SpellVisualKit: id, start anim, anim, head 3, chest 4, base 5, left hand 6, right hand 7, breath 8, weapons 9/10,
#      special 11..13, world 14, sound 15
def kit(kid, **eff):
    r = [0] * 20; r[0] = kid
    cols = dict(head=3, chest=4, base=5, lhand=6, rhand=7, breath=8, special=11)
    for k, v in eff.items(): r[cols[k]] = v
    return r
dbc("SpellVisualKit.dbc", 20, [kit(100, lhand=1001, rhand=1001), kit(101, base=1002), kit(102, chest=1004),
                               kit(103, special=1005), kit(104, base=1006), kit(105, chest=1009),
                               kit(106, base=1010), kit(107, rhand=1011)])
# ---- SpellVisualEffectName: id, name, file, area effect size, scale
dbc("SpellVisualEffectName.dbc", 5, [
    [1001, "FireHand", "Spells\\Fire_Hand.mdx", 0.0, 1.0],
    [1002, "FireCast", "Spells\\Fire_Cast_Base.mdx", 0.0, 1.0],
    [1003, "Fireball", "Spells\\Fireball_Missile.mdx", 0.0, 1.5],
    [1004, "FireImpact", "Spells\\Fire_Impact_Chest.mdx", 0.0, 1.0],
    [1005, "HolyPrecast", "Spells\\Holy_Precast_Chest.mdx", 0.0, 1.0],
    [1006, "HolyImpact", "Spells\\Holy_Impact_Base.mdx", 0.0, 1.0],
    [1007, "Frostbolt", "Spells\\Frostbolt_Missile.mdx", 0.0, 1.0],
    [1009, "Smite", "Spells\\Missing_Model.mdx", 0.0, 1.0],
    [1010, "Shield", "Spells\\Shield_State_Base.mdx", 0.0, 1.0],
    [1011, "DrainLife", "Spells\\Shadow_Channel_Hand.mdx", 0.0, 1.0],
])

# ---- the effect models: a small additive quad and one emitter each (a glow card on the fireball)
open(path("Spells\\Fire.blp"), "wb").write(blp(3))
open(path("Spells\\Glow_Fire.blp"), "wb").write(blp(7))
def effect(name, emitter, glow=False):
    q = [(-0.2, 0.0, 0.0), (0.2, 0.0, 0.0), (0.2, 0.0, 0.4), (-0.2, 0.0, 0.4)]
    v = [(p, (255, 0, 0, 0), (0, 0, 0, 0), (0, -1, 0), (0.0, 0.0)) for p in q]
    t = [0, 1, 2, 0, 2, 3]
    tex = [(0, "Spells\\Fire.blp"), (0, "Spells\\Glow_Fire.blp")]
    m = m2(name, v, t, [(0, 0, 6)], [(0, 0, 1 if glow else 0)], tex, [0, 1], [(0, 3)], [(-1, (0, 0, 0), {})],
           [(0, 0, 0, 1000, 0.0)], [], emitters=[emitter])
    open(path("Spells\\" + name + ".m2"), "wb").write(m)
fire = dict(pos=(0.0, 0.0, 0.1), texture=0, blend=4, rate=30.0, life=0.6, speed=0.5, gravity=-1.0, scales=(0.4, 0.3, 0.1),
            colors=[(255, 220, 120, 255), (255, 120, 30, 200), (80, 20, 10, 0)])
effect("Fire_Hand", fire)
effect("Fire_Cast_Base", dict(fire, rate=40.0, type=2, areaL=0.5))
effect("Fireball_Missile", dict(fire, rate=50.0, life=0.4, scales=(0.7, 0.5, 0.2)), glow=True)
effect("Fire_Impact_Chest", dict(fire, rate=60.0, type=2, speed=3.0))
effect("Holy_Precast_Chest", dict(fire, colors=[(255, 255, 200, 255), (255, 240, 160, 180), (200, 180, 80, 0)]))
effect("Holy_Impact_Base", dict(fire, rate=25.0))
effect("Frostbolt_Missile", dict(fire, colors=[(180, 220, 255, 255), (120, 180, 255, 200), (40, 80, 160, 0)]))
effect("Shield_State_Base", dict(fire, rate=15.0, type=2, areaL=1.0))
effect("Shadow_Channel_Hand", dict(fire, colors=[(120, 40, 160, 255), (80, 20, 120, 200), (20, 0, 40, 0)]))
# the level-up flash and the loot sparkle (not spells: fixed paths)
def special(rel, name):
    q = [(-0.2, 0.0, 0.0), (0.2, 0.0, 0.0), (0.2, 0.0, 0.4), (-0.2, 0.0, 0.4)]
    v = [(p, (255, 0, 0, 0), (0, 0, 0, 0), (0, -1, 0), (0.0, 0.0)) for p in q]
    m = m2(name, v, [0, 1, 2, 0, 2, 3], [(0, 0, 6)], [(0, 0, 0)], [(0, "Spells\\Fire.blp")], [0], [(0, 3)],
           [(-1, (0, 0, 0), {})], [(0, 0, 0, 1000, 0.0)], [], emitters=[dict(fire, rate=20.0)])
    open(path(rel), "wb").write(m)
special("Spells\\LevelUp\\LevelUp.m2", "LevelUp")
special("Particles\\LootFX.m2", "LootFX")

json.dump({"fireballs": fireballs, "heals": heals, "frostbolt": 116, "smite": 585, "speed_column": SPEED,
           "visuals": 5, "spells": len(fireballs) + len(heals) + 3, "models": 11, "failed": 1}, open(expect_path, "w"), indent=1)
print("spell fx data written to", out)
