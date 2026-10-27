"""Synthetic vanilla (v256) M2 creatures + DBCs for w3dconv --creatures.

    python3 gen_creature_data.py <Data dir> <expect.json>

Adds to a loose-file Data/ tree (use together with gen_wmo_data.py for the map):
  Creature/Testwolf/Testwolf.mdx (.m2 on disk)  3 bones: body, leg (rotates in walk), tail (global seq)
                                                 sequences: stand(0) 1000 ms, walk(4) 800 ms, dance(69), attack(16) 600 ms
  Creature/Testwolf/TestwolfSkin{Grey,Black}.blp skins (texture type 11)
  Character/Human/Male/HumanMale.mdx             geosets 0, 1, 5, 101, 102, 201, 301, 401, 402, 501, 701, 1301, 1501,
                                                 textures type 1 (body) and 6 (hair)
  Textures/BakedNpcTextures/HumanGuard.blp, Character/Human/Hair04_07.blp
  DBFilesClient/CreatureModelData, CreatureDisplayInfo, CreatureDisplayInfoExtra (19 fields), CharHairGeosets,
                CharacterFacialHairStyles, CharSections
The expectations (textures and geosets per display) are derived here independently.
"""
import json, os, struct, sys

out, exp_path = sys.argv[1], sys.argv[2]

def write(path, data):
    p = os.path.join(out, path.replace("\\", "/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data)

from m2_writer import Blob, blp, blp_rgba, m2

def box(cx, cy, cz, s, bone, w=(255, 0, 0, 0), idx=None):
    """8-vertex cube (12 triangles) skinned to 'bone'."""
    V = []
    for k in range(8):
        p = (cx + (s if k & 1 else -s), cy + (s if k & 2 else -s), cz + (s if k & 4 else -s))
        n = tuple(1 if p[i] > (cx, cy, cz)[i] else -1 for i in range(3))
        V.append((p, w, idx or (bone, 0, 0, 0), n, ((k & 1) * 1.0, (k >> 1 & 1) * 1.0)))
    T = [0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5]
    return V, T

import math
def qz(a): return (0.0, 0.0, math.sin(a / 2), math.cos(a / 2))
def qy(a): return (0.0, math.sin(a / 2), 0.0, math.cos(a / 2))

# --- wolf: body cube on bone 0 at the origin, leg cube on bone 1 (pivot at the hip), tail on bone 2
verts, tris = [], []
for (cx, cy, cz, s, b) in ((0, 0, 1, 0.5, 0), (0.3, 0, 0.3, 0.2, 1), (-0.8, 0, 1.2, 0.15, 2)):
    v, t = box(cx, cy, cz, s, b)
    base = len(verts); verts += v; tris += [base + i for i in t]
seqs = [(0, 0, 0, 1000, 0.0), (4, 0, 1000, 1800, 2.5), (69, 0, 1800, 2400, 0.0), (16, 0, 2400, 3000, 0.0)]
leg_rot = (-1, [(0, [(0, qy(0)), (1000, qy(0))]),                          # stand: still
                (4, [(1000, qy(0)), (1200, qy(0.6)), (1400, qy(0)), (1600, qy(-0.6)), (1800, qy(0))]),   # walk swing
                (69, [(1800, qy(0)), (2400, qy(0))]),
                (16, [(2400, qy(0)), (2700, qy(0.9)), (3000, qy(0))])])                # attack (unarmed): a kick
body_t = (-1, [(0, [(0, (0, 0, 0)), (500, (0, 0, 0.1)), (1000, (0, 0, 0))]),   # stand: breathing
               (4, [(1000, (0, 0, 0)), (1400, (0, 0, 0.05)), (1800, (0, 0, 0))]),
               (69, [(1800, (0, 0, 0)), (2400, (0, 0, 0))]),
               (16, [(2400, (0, 0, 0)), (3000, (0, 0, 0))])])
tail_r = (0, [(None, [(0, qz(0)), (500, qz(0.8)), (1000, qz(0))])])        # global sequence 0 (1000 ms)
wolf_bones = [(-1, (0, 0, 1), {"t": body_t}), (0, (0.3, 0, 0.6), {"r": leg_rot}), (0, (-0.6, 0, 1.2), {"r": tail_r})]
nt = len(tris)
wolf = m2("Testwolf", verts, tris, [(0, 0, nt - 36), (1, nt - 36, 36)], [(0, 0, 0), (1, 1, 1)],
          [(11, ""), (0, "Creature\\Testwolf\\TestwolfEyes.blp")], [0, 1], [(0, 0), (0, 1)], wolf_bones, seqs, [1000])
write("Creature/Testwolf/Testwolf.m2", wolf)
write("Creature/Testwolf/TestwolfSkinGrey.blp", blp(3))
write("Creature/Testwolf/TestwolfSkinBlack.blp", blp(5))
write("Creature/Testwolf/TestwolfEyes.blp", blp(7))

# --- human: one small cube per geoset on bone 0
# every clothing variant at once, like the real character models (kneepads, robe kilt, boots, ears, cape)
human_geosets = [0, 1, 5, 101, 102, 201, 301, 401, 402, 501, 502, 701, 702, 902, 903, 1301, 1302, 1501, 1502]
verts, tris, subs = [], [], []
for k, g in enumerate(human_geosets):
    v, t = box(0, k * 0.3, 1, 0.1, 0)
    base = len(verts); verts += v
    subs.append((g, len(tris), len(t))); tris += [base + i for i in t]
human = m2("HumanMale", verts, tris, subs, [(k, 0, 1 if g in (1, 5) else 0) for k, g in enumerate(human_geosets)],
           [(1, ""), (6, "")], [0, 1], [(0, 0)], [(-1, (0, 0, 0), {})], [(0, 0, 0, 1000, 0.0)], [],
           attachments=[(0, 0, (0.0, 0.4, 1.0)), (1, 0, (0.1, -0.3, 1.0)), (2, 0, (0.1, 0.3, 1.0)), (5, 0, (0.0, -0.2, 1.4)),
                        (6, 0, (0.0, 0.2, 1.4)), (11, 0, (0.0, 0.0, 1.7)), (11, 0, (9.0, 9.0, 9.0)), (77, 0, (1.0, 1.0, 1.0))])
write("Character/Human/Male/HumanMale.m2", human)
write("Textures/BakedNpcTextures/HumanGuard.blp", blp(9))
write("Character/Human/Hair04_07.blp", blp(11))

# ------------------------------------------------------------ DBCs
def dbc(fields, rows):
    strings = b"\0"; recs = b""
    def s_off(s):
        nonlocal strings
        o = len(strings); strings += s.encode() + b"\0"; return o
    for r in rows:
        assert len(r) == fields
        for v in r:
            if isinstance(v, str): recs += struct.pack("<I", s_off(v))
            elif isinstance(v, float): recs += struct.pack("<f", v)
            else: recs += struct.pack("<I", v & 0xFFFFFFFF)
    return b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, len(strings)) + recs + strings

write("DBFilesClient/CreatureModelData.dbc", dbc(6, [
    [11, 0, "Creature\\Testwolf\\Testwolf.mdx", 1, 1.0, 0],
    [49, 0, "Character\\Human\\Male\\HumanMale.mdx", 1, 1.0, 0],
    [77, 0, "Creature\\Missing\\Missing.mdx", 1, 1.0, 0]]))
write("DBFilesClient/CreatureDisplayInfo.dbc", dbc(11, [
    [1001, 11, 0, 0, 1.2, 255, "TestwolfSkinGrey", "", "", 0, 0],
    [1002, 11, 0, 0, 1.0, 255, "TestwolfSkinBlack", "", "", 0, 0],
    [1003, 49, 0, 500, 1.0, 255, "", "", "", 0, 0],
    [49, 49, 0, 0, 1.05, 255, "", "", "", 0, 0],             # the race's own display: its scale goes to CHRS
    [1004, 77, 0, 0, 1.0, 255, "", "", "", 0, 0]]))           # model file missing -> display failed
# NPC 500 wears a robe (chest item 7001: GeosetGroup3 = 1 -> kilt 1302) and boots (7002: GeosetGroup1 = 1 -> 502).
# Equipment slots from field 8: head, shoulder, shirt, chest(11), belt, legs, feet(14), wrist, gloves, tabard.
write("DBFilesClient/CreatureDisplayInfoExtra.dbc", dbc(19, [
    [500, 1, 0, 2, 3, 4, 7, 2] + [0, 0, 0, 7001, 0, 0, 7002, 0, 0, 0] + ["HumanGuard.blp"]]))
# ItemDisplayInfo, vanilla layout (23 fields): ID, models x2, textures x2, icon, GeosetGroup1..3, flags, spell visual, sound,
# HelmetGeosetVis x2, texture regions ArmUpper..Foot (14..21), item visual.
def idi(did, ml="", mr="", tl="", tr="", gg=(0, 0, 0), hv=(0, 0), regions=()):
    reg = list(regions) + [""] * (8 - len(regions))
    return [did, ml, mr, tl, tr, "", gg[0], gg[1], gg[2], 0, 0, 0, hv[0], hv[1]] + reg + [0]
write("DBFilesClient/ItemDisplayInfo.dbc", dbc(23, [
    idi(7001, gg=(0, 0, 1)), idi(7002, gg=(1, 0, 0)),
    idi(7101, gg=(2, 0, 0), regions=("TestRobe", "", "", "TestRobe", "Nothere")),       # chest: sleeves, torso art, one missing
    idi(7102, ml="TestHelm", tl="TestHelm", hv=(1, 2)),                                  # helmet: men keep hair, women lose it
    idi(7103, ml="TestSword", tl="TestSword"),                                            # weapon
    idi(7104, ml="TestShoulderL", mr="TestShoulderR", tl="TestShoulder", tr="TestShoulder"),   # shoulders
    idi(7105, tl="TestCape", gg=(1, 0, 0)),                                               # cloak
    idi(7106, ml="NoSuchModel", tl="NoSuchModel"),
    idi(7107, ml="TestHelm", tl="TestHelm", hv=(2, 2))]))                                      # model art missing -> no entry
write("DBFilesClient/HelmetGeosetVisData.dbc", dbc(3, [[1, 0, 0], [2, 0, 0x10]]))
def cube_m2(name):
    v, t = box(0, 0, 0.5, 0.3, 0)
    return m2(name, v, t, [(0, 0, len(t))], [(0, 0, 0)], [(2, "")], [0], [(0, 0)], [(-1, (0, 0, 0), {})],
              [(0, 0, 0, 1000, 0.0)], [])
IC = "Item\\ObjectComponents\\"
for path, tx in ((IC + "Weapon\\TestSword", 21), (IC + "Head\\TestHelm_HuM", 22), (IC + "Shoulder\\TestShoulderL", 23),
                 (IC + "Shoulder\\TestShoulderR", 23)):
    write(path + ".m2", cube_m2(path.split("\\")[-1]))
write(IC + "Weapon\\TestSword.blp", blp(21))
write(IC + "Head\\TestHelm_HuM.blp", blp(22))
write(IC + "Shoulder\\TestShoulder.blp", blp(23))
write(IC + "Cape\\TestCape.blp", blp(24))
TC = "Item\\TextureComponents\\"
write(TC + "ArmUpperTexture\\TestRobe_M.blp", blp_rgba(128, 64, lambda x, y: (200, 30, 30, 255)))
write(TC + "TorsoUpperTexture\\TestRobe_U.blp", blp_rgba(128, 64, lambda x, y: (30, 200, 30, 200)))
write("DBFilesClient/CharHairGeosets.dbc", dbc(6, [[1, 1, 0, 4, 5, 0], [2, 1, 0, 3, 1, 1]]))
write("DBFilesClient/CharacterFacialHairStyles.dbc", dbc(8, [[1, 0, 2, 2, 1, 1, 0, 0]]))
# ---- players (stage 5.5): CharSections rows of human male. (section, variation, colour, tex1, tex2);
# images are made of 2x2-aligned blocks so the converter's halving is exact and the composite can be
# predicted here pixel for pixel.
HM = "Character\\Human\\Male\\"
def quad(c):                      # 4 quadrant colours, (x, y) in the image
    return lambda x, y, w, h: c[(1 if x >= w // 2 else 0) + (2 if y >= h // 2 else 0)]
player_images = {
    HM + "HumanMaleSkin00_00.blp": (256, 256, quad([(200, 150, 120, 255), (190, 140, 110, 255), (210, 160, 130, 255), (170, 120, 90, 255)])),
    HM + "HumanMaleSkin00_01.blp": (256, 256, quad([(100, 70, 50, 255), (110, 80, 60, 255), (90, 60, 40, 255), (120, 90, 70, 255)])),
    HM + "HumanMaleFaceLower00_00.blp": (128, 64, lambda x, y, w, h: (60, 60, 200, 255) if y < 32 else (60, 200, 60, 136)),
    HM + "HumanMaleFaceUpper00_00.blp": (128, 32, lambda x, y, w, h: (250, 250, 0, 255) if x < 64 else (250, 0, 250, 0)),
    HM + "HumanMaleFaceLower01_01.blp": (128, 64, lambda x, y, w, h: (0, 0, 0, 255)),
    HM + "FacialLowerHair02_07.blp": (128, 64, lambda x, y, w, h: (90, 60, 30, 255) if y >= 32 else (0, 0, 0, 0)),
    HM + "HumanMaleNakedPelvisSkin00_00.blp": (128, 64, lambda x, y, w, h: (240, 240, 240, 255) if x < 96 else (0, 0, 255, 68)),
}
for path, (w, h, f) in player_images.items():
    write(path, blp_rgba(w, h, lambda x, y, f=f, w=w, h=h: f(x, y, w, h)))
write("Character/Human/Hair04_03.blp", blp(13))
cs_rows = [  # (section, variation, colour, tex1, tex2) for race 1 sex 0, in table order
    (3, 4, 7, "Character\\Human\\Hair04_07.blp", ""),
    (0, 0, 2, "Character\\Human\\Skin00_02.blp", ""),            # art missing: dropped by the converter
    (0, 0, 0, HM + "HumanMaleSkin00_00.blp", ""),
    (0, 0, 1, HM + "HumanMaleSkin00_01.blp", ""),
    (1, 0, 0, HM + "HumanMaleFaceLower00_00.blp", HM + "HumanMaleFaceUpper00_00.blp"),
    (1, 1, 1, HM + "HumanMaleFaceLower01_01.blp", ""),
    (2, 0, 7, "", ""),                                               # "no facial hair": no art, dropped
    (2, 2, 7, HM + "FacialLowerHair02_07.blp", ""),
    (4, 0, 0, HM + "HumanMaleNakedPelvisSkin00_00.blp", ""),
    (3, 4, 3, "Character\\Human\\Hair04_03.blp", ""),
]
write("DBFilesClient/CharSections.dbc", dbc(10, [[k + 1, 1, 0, sec, var, col, t1, t2, "", 0]
                                                 for k, (sec, var, col, t1, t2) in enumerate(cs_rows)]))

# ------------------------------------------------------------ expectations
def fnv(s):
    h = 2166136261
    for c in s.lower().replace("\\", "/"):
        h ^= ord(c); h = (h * 16777619) & 0xFFFFFFFF
    return h
# Geosets: WoWee bareCharacterGeosets + equipment (independent Python version of that rule):
# body 0, scalp, facial 1xx/2xx/3xx (the style's, else 101/201/301), forearms 401+gloves, shins 501+boots,
# ears 702, sleeves 801, kneepads 902, legs 1301 (+robe kilt), no-cape panel 1501; nothing else.
def wowee_geosets(scalp, f, robe=0, boots=0, cape=0):
    have = set(human_geosets)
    pick = lambda want, fb: want if want in have else (fb if fb in have else None)
    out = {0}
    out.add(pick(scalp, 1))
    for want, fb in zip(f, (101, 201, 301)):    # 0 = none of that feature
        if want: out.add(pick(want, fb))
    out.add(pick(401, 401)); out.add(pick(501 + boots, 501)); out.add(pick(702, 702)); out.add(pick(801, 801))
    out.add(pick(902, 902)); out.add(pick(1301 + robe, 1301) if robe else pick(1301, 1301))
    out.add(pick(1501 + cape - 1, 1502) if cape else pick(1501, 1501))
    return sorted(g for g in out if g is not None)
# Human display 1003: race 1 sex 0 hair style 4 colour 7 facial 2 -> scalp 5; facial 102, 201, 301; robe, boots
keep = wowee_geosets(5, (102, 201, 301), robe=1, boots=1)
exp = {
    "displays": {
        "1001": {"model": fnv("creature/testwolf/testwolf.m2"), "scale": 1.2, "skin1": fnv("Creature\\Testwolf\\TestwolfSkinGrey.blp")},
        "1002": {"model": fnv("creature/testwolf/testwolf.m2"), "scale": 1.0, "skin1": fnv("Creature\\Testwolf\\TestwolfSkinBlack.blp")},
        "1003": {"model": fnv("character/human/male/humanmale.m2"), "scale": 1.0,
                 "body": fnv("Textures\\BakedNpcTextures\\HumanGuard.blp"), "hair": fnv("Character\\Human\\Hair04_07.blp"),
                 "geosets": keep}},
    "missing_displays": [1004],
    "wolf_seqs": [0, 4, 16, 69],   # in the converter's priority order; the dance (69) is kept since the animation stage
}

# ---- players: resolve looks with the rules of WoWee resolveCharacterSections (+ the documented fallbacks),
# then composite the 128x128 body texture exactly as the 3DS must.
present = lambda p: p and (p in player_images or p.endswith(("Hair04_07.blp", "Hair04_03.blp")))
rows = [r for r in cs_rows if present(r[3]) or present(r[4])]
def resolve(skin, face, hs, hc, fh):
    first = lambda pred: next((r for r in rows if pred(r)), None)
    sk = first(lambda r: r[0] == 0 and r[2] == skin) or first(lambda r: r[0] == 0)
    fc = first(lambda r: r[0] == 1 and r[1] == face and r[2] == skin) or first(lambda r: r[0] == 1 and (r[1] == face or r[2] == skin))
    hr = first(lambda r: r[0] == 3 and r[1] == hs and r[2] == hc) or first(lambda r: r[0] == 3 and r[1] == hs) or first(lambda r: r[0] == 3)
    un = first(lambda r: r[0] == 4 and r[2] == skin) or first(lambda r: r[0] == 4)
    fa = first(lambda r: r[0] == 2 and r[1] == fh and r[2] == hc) or first(lambda r: r[0] == 2 and r[1] == fh)
    layers = []   # (path, x, y) in the 256 atlas, drawing order: base, pelvis, torso, face lower/upper, facial lower/upper
    if sk: layers.append((sk[3], 0, 0))
    if un and un[3]: layers.append((un[3], 128, 96))
    if fc and fc[3]: layers.append((fc[3], 0, 192))
    if fc and fc[4]: layers.append((fc[4], 0, 160))
    if fa and fa[3]: layers.append((fa[3], 0, 192))
    scalp = 1
    for r in [[1, 0, 4, 5, 0], [1, 0, 3, 1, 1]]:   # CharHairGeosets rows of human male (race, sex, var, geoset, scalp)
        if r[2] == hs: scalp = 1 if r[4] else r[3]
    f = (0, 0, 0)                                   # players: no style row -> no facial hair geometry
    if fh == 2: f = (102, 201, 301)                 # CharacterFacialHairStyles (1, 0, 2): 100+2, 200+1, 300+1
    return layers, (hr[3] if hr else ""), scalp, f
def geosets_for(scalp, f): return wowee_geosets(scalp, f)
item_images = {
    TC + "ArmUpperTexture\\TestRobe_M.blp": (128, 64, lambda x, y, w, h: (200, 30, 30, 255)),
    TC + "TorsoUpperTexture\\TestRobe_U.blp": (128, 64, lambda x, y, w, h: (30, 200, 30, 200)),
}
def composite(layers):
    A = 128
    img = [[(0x80, 0x80, 0x80)] * A for _ in range(A)]
    q = lambda v: (v * 15 + 127) // 255 * 17
    for li, (path, X, Y) in enumerate(layers):
        w, h, f = {**player_images, **item_images}[path]
        for y in range(h // 2):
            for x in range(w // 2):
                r, g, b, a = f(2 * x, 2 * y, w, h)        # 2x2 blocks are uniform: the halved pixel
                r, g, b, a = q(r), q(g), q(b), (255 if li == 0 else q(a))
                ax, ay = X // 2 + x, Y // 2 + y
                o = img[ay][ax]
                img[ay][ax] = tuple((c * a + oc * (255 - a) + 127) // 255 for c, oc in zip((r, g, b), o))
    return [((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3) for row in img for p in row]   # row-major 565
looks = [dict(skin=0, face=0, hs=4, hc=7, fh=2), dict(skin=1, face=1, hs=3, hc=3, fh=0), dict(skin=2, face=5, hs=4, hc=3, fh=2)]
exp["players"] = {"race": 1, "sex": 0, "model": fnv("character/human/male/humanmale.m2"), "scale": 1.05,
                  "races_ok": 1, "races_failed": 15, "looks": []}
for L in looks:
    layers, hair, scalp, f = resolve(L["skin"], L["face"], L["hs"], L["hc"], L["fh"])
    exp["players"]["looks"].append(dict(L, hair=fnv(hair) if hair else 0, geosets=geosets_for(scalp, f),
                                        layers=len(layers), image=composite(layers)))
# worn armour (stage 5.19): look 0 with items. Layers after the skin ones are the items', in W3D_LAYER_SLOTS order.
base_layers, _, _, _ = resolve(0, 0, 4, 7, 2)
ARM = TC + "ArmUpperTexture\\TestRobe_M.blp"; TOR = TC + "TorsoUpperTexture\\TestRobe_U.blp"
fnv_cape = fnv("Item\\ObjectComponents\\Cape\\TestCape.blp")
f0 = (102, 201, 301)
def equip_case(slots, ids, extra_layers, scalp, robe=0, boots=0, cape=0, capetex=0):
    layers = base_layers + extra_layers
    return dict(look=0, slots=slots, ids=ids, layers=len(layers), capetex=capetex,
                geosets=wowee_geosets(scalp, f0, robe=robe, boots=boots, cape=cape), image=composite(layers))
exp["equips"] = [
    equip_case([4, 7], [7101, 7002], [(ARM, 0, 0), (TOR, 128, 0)], 5, boots=1),            # chest art + boots
    equip_case([0, 14], [7107, 7105], [], 1, cape=3, capetex=fnv_cape),                      # helmet hides hair; cloak
    equip_case([3, 4], [7101, 7001], [(ARM, 0, 0), (TOR, 128, 0)], 5, robe=1),               # shirt art under a robe
    equip_case([0, 3], [7102, 7101], [(ARM, 0, 0), (TOR, 128, 0)], 5),                       # helmet that leaves the man's hair
]
json.dump(exp, open(exp_path, "w"))
print("creature data written; human geosets expected:", sorted(keep))
