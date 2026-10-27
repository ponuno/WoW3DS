"""Checks ITEM / CIMG / MODL entries and the human model's attachments of a pack built from gen_creature_data.py."""
import struct, sys
b = open(sys.argv[1], "rb").read()
magic, ver, n, idx = struct.unpack_from("<4I", b, 0)
E = {}
for i in range(n):
    t, eid, off, size = struct.unpack_from("<4I", b, idx + i * 16)
    E[(t, eid)] = (off, size)
F = lambda s: struct.unpack("<I", s.encode())[0]
bad = 0
def check(cond, what):
    global bad
    print("ok  " if cond else "FAIL", what); bad += not cond
check(ver == 5, f"pack version {ver}")
def item(did):
    k = (F("ITEM"), did)
    if k not in E: return None
    off, size = E[k]
    assert size == 148, size
    v = struct.unpack_from("<4B16I2I2I16I", b, off)
    return dict(gg=v[0:3], hide=v[3], img=[v[4:12], v[12:20]], cape=v[20:22], model=v[22:24], helm=v[24:40])
def cimg(i):
    off, size = E[(F("CIMG"), i)]
    return struct.unpack_from("<4H", b, off), size
# 7101 chest: sleeves 2, ArmUpper _M only for men, TorsoUpper _U for both, the other regions empty
it = item(7101)
check(it is not None and it["gg"] == (2, 0, 0), "7101 present, gg1 = 2")
if it:
    a, t = it["img"][0][0], it["img"][1][0]
    check(a != 0 and it["img"][1][0] == 0, "7101 ArmUpper art for men only")
    check(it["img"][0][3] != 0 and it["img"][0][3] == it["img"][1][3], "7101 TorsoUpper _U shared")
    check(sum(1 for s in it["img"] for x in s if x) == 3, "7101 exactly 3 layers")
    check(cimg(it["img"][0][0]) == ((64, 32, 0, 0), 8 + 64 * 32 * 2), f"ArmUpper layer {cimg(it['img'][0][0])}")
    check(cimg(it["img"][0][3]) == ((64, 32, 64, 0), 8 + 64 * 32 * 2), "TorsoUpper layer at (64,0)")
it = item(7102)
check(it is not None and it["helm"][0] != 0 and not any(it["helm"][1:]), "7102 helmet only for human male")
check(it is not None and it["hide"] == 2, f"7102 hides the women's hair only ({it and it['hide']})")
if it: check((F("MODL"), it["helm"][0]) in E, "7102 helmet MODL packed")
it = item(7103)
check(it is not None and it["model"][0] != 0 and it["model"][1] == 0 and (F("MODL"), it["model"][0]) in E, "7103 weapon model")
it = item(7104)
check(it is not None and it["model"][0] != 0 and it["model"][1] != 0 and it["model"][0] != it["model"][1], "7104 two shoulder models")
it = item(7105)
check(it is not None and it["cape"][0] != 0 and it["cape"][0] == it["cape"][1] and (F("TEX "), it["cape"][0]) in E and it["gg"] == (1, 0, 0), "7105 cloak texture")
check(item(7106) is None, "7106 (no art) has no entry")
check(item(7001) is not None and item(7001)["gg"] == (0, 0, 1), "7001 geoset-only entry")
# human CMDL attachments
import json
exp = json.load(open(sys.argv[2]))
mid = exp["displays"]["1003"]["model"]
off, size = E[(F("CMDL"), mid)]
base = 14 * 4 + 8 * 4 + 8 * 4          # floats + counts + offsets
sb, = struct.unpack_from("<h", b, off + base)
ac, ao = struct.unpack_from("<II", b, off + base + 4 + 4 * 3 + 4 + 0) if False else (None, None)
ac, ao = struct.unpack_from("<II", b, off + base + 2 + 2 + 12)
check(ac == 6, f"human attachments {ac}")
ids = [struct.unpack_from("<Hh3f", b, off + ao + 16 * k) for k in range(ac)]
print("   ", [(i[0], i[1], tuple(round(x, 2) for x in i[2:])) for i in ids])
check([i[0] for i in ids] == [0, 1, 2, 5, 6, 11] and ids[5][4] == 1.7 or abs(ids[5][4] - 1.7) < 1e-5, "ids kept, first of a duplicate wins, unknown dropped")
print("ITEM PACK:", "PASS" if not bad else f"{bad} FAILED"); sys.exit(1 if bad else 0)
