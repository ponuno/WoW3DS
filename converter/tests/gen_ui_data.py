"""Synthetic client data for w3dconv --ui (UI stage 7): Interface\\* pictures as BLP2 (A8R8G8B8), the DBCs that name
them (SpellIcon, Spell, ItemDisplayInfo, WorldMapArea, WorldMapOverlay, TalentTab, Talent, TaxiNodes, TaxiPath) and a
Latin-only Fonts\\FRIZQT__.TTF, so the converter's icons, skin, fonts (with the Cyrillic fallback) and maps can be
checked without a real client.
  gen_ui_data.py outdir latin_only.ttf"""
import math, os, shutil, struct, sys

out, ttf = sys.argv[1], sys.argv[2]

def blp(path, w, h, px):
    """px(x, y) -> (r, g, b, a)"""
    data = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px(x, y)
            data += bytes((b, g, r, a))
    hdr = b"BLP2" + struct.pack("<I", 1) + bytes((3, 8, 0, 0)) + struct.pack("<II", w, h)
    offs = [1172] + [0] * 15
    sizes = [len(data)] + [0] * 15
    hdr += struct.pack("<16I", *offs) + struct.pack("<16I", *sizes) + b"\0" * 1024
    assert len(hdr) == 1172
    full = os.path.join(out, *path.split("\\"))
    os.makedirs(os.path.dirname(full), exist_ok=True)
    open(full, "wb").write(hdr + bytes(data))

def dbc(name, fields, rows, strings):
    recs = b"".join(struct.pack("<" + "".join("f" if isinstance(v, float) else "i" if v < 0 else "I" for v in r), *r) for r in rows)
    os.makedirs(os.path.join(out, "DBFilesClient"), exist_ok=True)
    open(os.path.join(out, "DBFilesClient", name), "wb").write(
        b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, len(strings)) + recs + strings)

class Strings:
    def __init__(self): self.b = b"\0"
    def __call__(self, t):
        if not t: return 0
        off = len(self.b); self.b += t.encode() + b"\0"; return off

# ---- icons: a coloured square with a bevel (the converter trims 5/64) and a diagonal stripe
def icon(path, rgb):
    def px(x, y):
        if x < 5 or y < 5 or x >= 59 or y >= 59: return (255, 0, 255, 255)   # bevel: must be trimmed away
        if abs(x - y) < 4: return (255, 255, 255, 255)
        return rgb + (255,)
    blp(path, 64, 64, px)

ICONS = {"Spell_Fire_Fireball": (220, 60, 20), "Ability_Warrior_HeroicStrike": (160, 40, 40), "INV_Misc_Bag_08": (120, 80, 40),
         "INV_Sword_04": (140, 140, 160), "INV_Misc_Map_01": (200, 180, 120), "INV_Misc_Book_09": (80, 40, 120),
         "INV_Misc_Note_01": (220, 210, 170), "INV_Letter_01": (230, 230, 230), "Spell_Holy_PrayerOfFortitude": (240, 240, 120),
         "INV_Misc_Gear_01": (110, 110, 110), "INV_Misc_QuestionMark": (60, 60, 200), "INV_Misc_Food_15": (200, 140, 60),
         "INV_Shirt_05": (60, 160, 60)}
for n, c in ICONS.items():
    icon("Interface\\Icons\\" + n + ".blp", c)
# (Ability_DualWield missing on purpose: tab_combat falls back to INV_Sword_04)

# ---- skin: the tooltip border strip (8 squares 16x16; left red, right green, top blue, bottom yellow, corners white)
cols = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (255, 255, 255), (200, 200, 200), (150, 150, 150), (100, 100, 100)]
# like the real file: the outer side of the right edge / bottom edge squares is their right side (the bottom one is
# stored transposed), the left / top ones have it on the left; corners are drawn whole
def border(x, y):
    k, u = x // 16, x % 16
    if k >= 4: return cols[k] + (255,)
    return cols[k] + (255 if (u >= 10 if k in (1, 3) else u < 6) else 0,)
blp("Interface\\Tooltips\\UI-Tooltip-Border.blp", 128, 16, border)
blp("Interface\\DialogFrame\\UI-DialogBox-Border.blp", 256, 32, lambda x, y: (200, 160, 40, 255 if (x % 32) < 12 else 0))
for state, c in (("Up", (150, 20, 20)), ("Down", (100, 10, 10)), ("Disabled", (80, 80, 80))):
    blp("Interface\\Buttons\\UI-Panel-Button-%s.blp" % state, 128, 32, lambda x, y, c=c: c + (255,) if x < 80 and y < 22 else (0, 0, 0, 0))
blp("Interface\\Buttons\\UI-Quickslot2.blp", 64, 64, lambda x, y: (90, 90, 90, 255 if min(x, y, 63 - x, 63 - y) < 14 else 0))
blp("Interface\\TargetingFrame\\UI-StatusBar.blp", 256, 16, lambda x, y: (255 - y * 8, 255 - y * 8, 255 - y * 8, 255))
for g in ("GossipGossipIcon", "VendorGossipIcon", "TaxiGossipIcon", "TrainerGossipIcon", "BinderGossipIcon", "BankerGossipIcon",
          "AvailableQuestIcon", "ActiveQuestIcon"):
    blp("Interface\\GossipFrame\\%s.blp" % g, 16, 16, lambda x, y: (250, 220, 0, 255 if (x - 8) ** 2 + (y - 8) ** 2 < 40 else 0))
for m, c in (("Gold", (255, 210, 0)), ("Silver", (200, 200, 210)), ("Copper", (190, 110, 50))):
    blp("Interface\\MoneyFrame\\UI-%sIcon.blp" % m, 16, 16, lambda x, y, c=c: c + (255 if (x - 8) ** 2 + (y - 8) ** 2 < 36 else 0,))
blp("Interface\\Minimap\\MinimapArrow.blp", 32, 32, lambda x, y: (255, 255, 0, 255 if y > 4 and abs(x - 16) < (y - 4) / 2 and y < 28 else 0))
for s in ("Head", "Neck", "Shoulder", "Chest", "Feet", "MainHand", "Bag"):
    blp("Interface\\PaperDoll\\UI-PaperDoll-Slot-%s.blp" % s, 64, 64, lambda x, y: (40, 40, 50, 255) if (x + y) % 16 else (90, 90, 110, 255))
blp("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Classes.blp", 256, 256, lambda x, y: ((x // 64) * 70, (y // 64) * 70, 120, 255))
# stage 5.29: glue screens (a logo, red glue buttons, a gold border, an edit box, arrows, race / gender sheets)
blp("Interface\\Glues\\Common\\Glues-WoW-Logo.blp", 256, 128,
    lambda x, y: (255, 200, 40, 255) if 20 < y < 108 and (x // 16 + y // 16) % 2 else (0, 0, 0, 0))
for state, c in (("Up", (170, 20, 10)), ("Down", (120, 10, 5)), ("Disabled", (80, 70, 70)), ("Highlight", (255, 255, 160))):
    blp("Interface\\Glues\\Common\\Glue-Panel-Button-%s.blp" % state, 128, 32,
        lambda x, y, c=c: (c + (255,) if 2 < x < 78 and 2 < y < 20 else (230, 180, 60, 255)) if x < 80 and y < 22 else (0, 0, 0, 0))
blp("Interface\\Glues\\Common\\Glue-Tooltip-Border.blp", 128, 16, lambda x, y: (240, 200, 60, 255 if (x % 16) < 3 or y < 3 else 0))
blp("Interface\\Common\\Common-Input-Border.blp", 128, 32,
    lambda x, y: (150, 150, 150, 255) if y < 20 and (x < 4 or x > 123 or y < 3 or y > 16) else (0, 0, 0, 0))
for side in ("Left", "Right"):
    blp("Interface\\Glues\\Common\\Glue-%sArrow-Button-Up.blp" % side, 32, 32,
        lambda x, y, sd=side: (255, 210, 0, 255 if (x > 16 - abs(y - 16) if sd == "Left" else x < 16 + abs(y - 16)) and 6 < x < 26 and 6 < y < 26 else 0))
blp("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Races.blp", 256, 256,
    lambda x, y: (60 + (x // 64) * 50, 40 + (y // 64) * 50, 200 - (y // 128) * 100, 255))
blp("Interface\\Glues\\CharacterCreate\\UI-CharacterCreate-Gender.blp", 128, 64, lambda x, y: ((80, 120, 255, 255) if x < 64 else (255, 120, 180, 255)))

# ---- world maps: Elwynn (zone, 12 tiles) with one overlay, Azeroth (continent); Westfall has only 3 tiles (skipped)
def mapTile(name, i, base):
    def px(x, y):
        gx, gy = (i % 4) * 256 + x, (i // 4) * 256 + y
        if gx % 128 == 0 or gy % 128 == 0: return (40, 40, 40, 255)
        return (base[0], min(255, base[1] + gx // 8), min(255, base[2] + gy // 8), 255)
    blp("Interface\\WorldMap\\%s\\%s%d.blp" % (name, name, i + 1), 256, 256, px)
for i in range(12): mapTile("Elwynn", i, (40, 90, 30))
for i in range(12): mapTile("Azeroth", i, (20, 40, 90))
for i in range(3): mapTile("Westfall", i, (90, 90, 30))
blp("Interface\\WorldMap\\Elwynn\\Goldshire1.blp", 128, 128, lambda x, y: (255, 0, 0, 255) if x < 100 and y < 80 else (0, 0, 0, 0))

S = Strings()
dbc("SpellIcon.dbc", 2, [[1, S("Interface\\Icons\\Spell_Fire_Fireball")], [2, S("Interface\\Icons\\Ability_Warrior_HeroicStrike")],
                         [3, S("Interface\\Icons\\Missing_Icon")]], S.b)
S = Strings()
spells = []
def spell(i, name, icon, effect=0, trigger=0):
    r = [0] * 173
    r[0] = i; r[120] = S(name); r[117] = icon; r[61] = effect; r[109] = trigger
    spells.append(r)
spell(133, "Fireball", 1)
spell(78, "Heroic Strike", 2)
spell(284, "Heroic Strike", 2)
spell(7380, "Heroic Strike", 0, effect=36, trigger=284)   # teaches 284: shows its icon
spell(555, "Broken", 3)                                    # its picture does not exist
spell(2366, "Herb Gathering", 1, effect=33); spells[-1][106] = 2      # OPEN_LOCK, LockType 2 (herbalism)
dbc("Spell.dbc", 173, spells, S.b)
# Lock.dbc: ID, Type[8], Index[8], Skill[8], Action[8] (33 fields); 99 = a herb needing herbalism 50
lk = [99] + [2] + [0] * 7 + [2] + [0] * 7 + [50] + [0] * 7 + [0] * 8
dbc("Lock.dbc", 33, [lk], b"\0")
# AreaTrigger.dbc: ID, Map, X, Y, Z, Radius, Box length / width / height / yaw (10 fields): a sphere and a turned box
dbc("AreaTrigger.dbc", 10, [[2173, 369, -4.0, 5.0, 6.0, 3.5, 0.0, 0.0, 0.0, 0.0], [45, 0, 100.0, 200.0, 30.0, 0.0, 10.0, 4.0, 6.0, 1.5708]], b"\0")
S = Strings()
idi = []
for did, n in ((1001, "INV_Misc_Food_15"), (1002, "INV_Shirt_05"), (1003, "INV_Misc_Food_15"), (1004, "")):
    r = [0] * 23
    r[0] = did; r[5] = S(n)
    idi.append(r)
dbc("ItemDisplayInfo.dbc", 23, idi, S.b)
S = Strings()
dbc("WorldMapArea.dbc", 8, [[1, 0, 0, S("Azeroth"), 16000.0, -22569.0, 11733.0, -14000.0],
                            [14, 0, 12, S("Elwynn"), 1535.0, -1935.0, -7939.0, -10254.0],
                            [15, 0, 40, S("Westfall"), 500.0, -2000.0, -9000.0, -12000.0]], S.b)
S = Strings()
dbc("WorldMapOverlay.dbc", 17, [[1, 14, 87, 0, 0, 0, 0, 0, S("Goldshire"), 100, 80, 300, 400, 0, 0, 0, 0]], S.b)
S = Strings()
dbc("TalentTab.dbc", 16, [[161, S("Arms")] + [0] * 10 + [1, 0, 0, S("WarriorArms")],
                          [164, S("Fury")] + [0] * 10 + [1, 0, 1, S("WarriorFury")]], S.b)
dbc("Talent.dbc", 21, [[124, 161, 0, 0, 12282, 12663, 12664, 0, 0, 0, 0, 0, 0, 0, 0] + [0] * 6,
                       [130, 161, 1, 1, 12286, 12658, 0, 0, 0, 124, 0, 0, 2, 0, 0] + [0] * 6], b"\0")
S = Strings()
dbc("TaxiNodes.dbc", 16, [[2, 0, -8835.0, 490.0, 109.0, S("Stormwind, Elwynn")] + [0] * 10,
                          [4, 0, -10628.0, 1036.0, 34.0, S("Sentinel Hill, Westfall")] + [0] * 10], S.b)
dbc("TaxiPath.dbc", 4, [[1, 2, 4, 150], [2, 4, 2, 150]], b"\0")

os.makedirs(os.path.join(out, "Fonts"), exist_ok=True)
shutil.copy(ttf, os.path.join(out, "Fonts", "FRIZQT__.TTF"))
print("synthetic UI data in", out)
