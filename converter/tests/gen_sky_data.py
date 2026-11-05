"""6.60: Light.dbc + LightIntBand.dbc for the sky colours (w3dconv --texts-only -> sky.txt; proto3ds/tests/sky_test.cpp reads it).
  Map 0: a global light (LightParams 1: the sky top goes from dark blue at midnight to blue at noon; every other channel is constant) and
         a zone light at world (-8800, -100), inner radius 100 yd, outer 300 yd (LightParams 2: red, constant).
  Map 1: a global light only (LightParams 3: green, constant).
  gen_sky_data.py <data dir>"""
import os, struct, sys
TS = 533.33333
ZP = 32 * TS
out = sys.argv[1]
def write(path, data):
    p = os.path.join(out, path); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "wb").write(data)
def dbc(fields, rows):
    recs = b"".join(b"".join(struct.pack("<f", v) if isinstance(v, float) else struct.pack("<I", v) for v in r) for r in rows)
    return b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, 1) + recs + b"\0"
wx, wy = -8800.0, -100.0
dbcX, dbcY = (ZP - wy) * 36, (ZP - wx) * 36     # lightPositionToWorld inverted: world x = ZP - dbcY / 36, world y = ZP - dbcX / 36
light = [
    [1, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 1, 0, 0, 0, 0],
    [2, 0, dbcX, 0.0, dbcY, 100.0 * 36, 300.0 * 36, 2, 0, 0, 0, 0],
    [3, 1, 0.0, 0.0, 0.0, 0.0, 0.0, 3, 0, 0, 0, 0],
]
write("DBFilesClient/Light.dbc", dbc(12, light))
def bgra(r, g, b): return (r << 16) | (g << 8) | b
def band(params, ch, keys):
    times = [k[0] for k in keys] + [0] * (16 - len(keys))
    vals = [bgra(*k[1]) for k in keys] + [0] * (16 - len(keys))
    return [(params - 1) * 18 + ch + 1, len(keys)] + times + vals
rows = []
const = {1: {3: (110, 155, 215), 4: (150, 185, 225), 5: (175, 200, 228), 7: (190, 210, 230)},
         2: {2: (200, 40, 40), 3: (180, 50, 50), 4: (160, 60, 60), 5: (140, 70, 70), 7: (120, 80, 80)},
         3: {2: (20, 200, 20), 3: (30, 180, 30), 4: (40, 160, 40), 5: (50, 140, 50), 7: (60, 120, 60)}}
rows.append(band(1, 2, [(0, (0, 0, 16)), (1440, (70, 120, 200))]))   # top: midnight .. noon (half minutes)
for params, chans in const.items():
    for ch, c in chans.items(): rows.append(band(params, ch, [(1440, c)]))
write("DBFilesClient/LightIntBand.dbc", dbc(34, rows))

# ---- 6.61: sky models. LightParams.dbc (column 2 = LightSkybox id): sets 1 (the global light of map 0) and 2 (the zone) have none / a sky
#      model 5; set 3 (map 1) the model 6, whose file is missing from the client. LightSkybox.dbc: id, model path.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m2_writer import m2, blp
def dbc_s(fields, rows):
    strs = bytearray(b"\0"); offs = {}
    recs = b""
    for r in rows:
        r = list(r) + [0] * (fields - len(r))
        for v in r:
            if isinstance(v, str):
                if v not in offs: offs[v] = len(strs); strs.extend(v.encode() + b"\0")
                recs += struct.pack("<I", offs[v])
            else: recs += struct.pack("<I", v)
    return b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, len(strs)) + recs + bytes(strs)
write("DBFilesClient/LightParams.dbc", dbc_s(16, [[1, 0, 0], [2, 0, 5], [3, 0, 6]]))
write("DBFilesClient/LightSkybox.dbc", dbc_s(2, [[5, "World\\Skyboxes\\Test_Sky.mdx"], [6, "World\\Skyboxes\\Missing_Sky.mdx"]]))
write("Textures/Sky/Test_Sky.blp", blp(5))
q = [(-100.0, -100.0, 20.0), (100.0, -100.0, 20.0), (100.0, 100.0, 20.0), (-100.0, 100.0, 20.0), (0.0, 0.0, 120.0)]
v = [(p, (255, 0, 0, 0), (0, 0, 0, 0), (0, 0, -1), (0.0, 0.0)) for p in q]
t = [0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4]
write("World/Skyboxes/Test_Sky.m2", m2("Test_Sky", v, t, [(0, 0, 12)], [(0, 0, 0)], [(0, "Textures\\Sky\\Test_Sky.blp")], [0], [(0, 3)],
                                       [(-1, (0, 0, 0), {})], [(0, 0, 0, 1000, 0.0)], []))
