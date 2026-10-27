"""Synthetic game data for the liquids of w3dconv (stage 5.26; no client files needed).

    python3 gen_water_data.py <out Data dir> <expect.txt>

Writes a loose-file Data/ tree:
  World/Maps/Test/Test_32_48.adt, Test_33_48.adt   flat terrain at z 80 with MCLQ in the real (vanilla) layout:
      chunk (2,3)  river at 78 over a basin at 70, cell (0,0) hidden (0x0F)
      chunk (2,4)  two layers: river at 78 on columns 0-3, magma at 79.5 on columns 2-7 (the higher one wins)
      chunk (5,5)  ocean at 0 over ground at -20, every cell "dark water" (0x81: deep)
      chunk (6,6)  a sloping river: 78 - 0.5 * row
      chunk (7,7)  the river flag set but every cell hidden: dry
      33_48 chunk (1,1) river at 60
  World/wmo/Test/Pool.wmo + Pool_000.wmo (water, 3 x 2 MLIQ cells, one hidden) + Pool_001.wmo (lava, group liquid 2)
      placed twice: turned 30 degrees inside 32_48, and unturned across the 32_48 / 33_48 border
  XTextures/river/lake_a.1..30.blp (grey ripples), XTextures/ocean/ocean_h.1..5.blp (blue: only 5 frames exist)
  DBFilesClient/Map.dbc, Light.dbc (a global light + a zone light over 33_48), LightIntBand.dbc, LightFloatBand.dbc
expect.txt holds what the pack must say, computed here independently:
  Q x y wet flags level        liquid query at render (x, y) (flags = W3D_LIQ_* byte, level only when wet)
  F liquid frames               animation frames of the liquid's batches
  L tileX tileY kind cr cg cb fr fg fb fogr fogg fogb fogEnd   LIQD look of the tile
  T tileX tileY liquid tris     WATR triangles of that liquid in the tile (merged quads)
"""
import math, os, struct, sys

TS = 533.33333
ZP = 32 * TS
CELL = TS / 128
out, expect_path = sys.argv[1], sys.argv[2]
exp = []

def chunk(magic, payload):
    return magic[::-1].encode() + struct.pack("<I", len(payload)) + payload

def write(path, data):
    p = os.path.join(out, path.replace("\\", "/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data)

# ------------------------------------------------------------ textures (BLP2 palettised, no alpha)
def blp(path, w, pix_fn, pal_fn):
    pal = b"".join(struct.pack("<BBBB", *pal_fn(i), 255) for i in range(256))   # BGRA
    pix = bytes(pix_fn(x, y) for y in range(w) for x in range(w))
    hdr = b"BLP2" + struct.pack("<IBBBBII", 1, 1, 0, 0, 0, w, w)
    offs = [0] * 16; sizes = [0] * 16
    offs[0] = len(hdr) + 64 + 64 + 1024; sizes[0] = len(pix)
    write(path, hdr + struct.pack("<16I", *offs) + struct.pack("<16I", *sizes) + pal + pix)

for f in range(1, 31):
    blp(f"XTextures/river/lake_a.{f}.blp", 32, lambda x, y, f=f: (x * 5 + y * 3 + f * 8) % 256,
        lambda i: (100 + i // 3, 100 + i // 3, 100 + i // 3))                         # grey: takes the Light colour
for f in range(1, 6):
    blp(f"XTextures/ocean/ocean_h.{f}.blp", 32, lambda x, y, f=f: (x * 3 + y * 7 + f * 8) % 256,
        lambda i: (120 + i // 4, 60 + i // 8, 10 + i // 16))                          # BGR: saturated blue
blp("Textures/Test/Wall.blp", 8, lambda x, y: (x + y) % 256, lambda i: (90, 90, 90))

# ------------------------------------------------------------ DBCs
def dbc(fields, rows, strings=b"\0"):
    recs = b""
    for r in rows:
        assert len(r) == fields
        recs += b"".join(struct.pack("<f", v) if isinstance(v, float) else struct.pack("<I", v & 0xFFFFFFFF) for v in r)
    return b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, len(strings)) + recs + strings

write("DBFilesClient/Map.dbc", dbc(3, [[0, 1, 0], [43, 6, 0]], b"\0Test\0Grotto\0"))
# Light: ID, map, x, z, y (36ths of a yard, x/y on the tile grid's axes), inner, outer, params clear, rain, underwater, + 2
zone_c = (ZP - (32 - 48 - 0.5) * TS, ZP - (32 - 33 - 0.5) * TS)   # dbc (x, y) of the centre of tile 33_48 ...
# lightPositionToWorld: world x = ZP - dbcY / 36, world y = ZP - dbcX / 36  ->  dbcY = (ZP - wx) * 36, dbcX = (ZP - wy) * 36
cx33, cy33 = (32 - 48 - 0.5) * TS, (32 - 33 - 0.5) * TS            # render centre of tile 33_48
dbcX, dbcY = (ZP - cy33) * 36, (ZP - cx33) * 36
write("DBFilesClient/Light.dbc", dbc(12, [[1, 0, 0.0, 0.0, 0.0, 0.0, 0.0, 1, 0, 2, 0, 0],
                                          [2, 0, dbcX, 0.0, dbcY, 100.0 * 36, 300.0 * 36, 3, 0, 4, 0, 0]]))
def bgra(r, g, b): return (r << 16) | (g << 8) | b
def int_band(params, ch, keys):         # keys: [(half minutes, (r, g, b))]
    times = [k[0] for k in keys] + [0] * (16 - len(keys))
    vals = [bgra(*k[1]) for k in keys] + [0] * (16 - len(keys))
    return [(params - 1) * 18 + ch + 1, len(keys)] + times + vals
def float_band(params, ch, keys):
    times = [k[0] for k in keys] + [0] * (16 - len(keys))
    vals = [float(k[1]) for k in keys] + [0.0] * (16 - len(keys))
    return [(params - 1) * 6 + ch + 1, len(keys)] + times + vals
ib = [
    int_band(1, 16, [(0, (0, 0, 0)), (1440, (90, 150, 170))]),   # river shallow at noon (interpolated key)
    int_band(1, 17, [(1440, (20, 50, 70))]),                     # river deep
    int_band(1, 14, [(1440, (60, 110, 160))]),                   # ocean shallow
    int_band(1, 15, [(1440, (10, 30, 80))]),                     # ocean deep
    int_band(2, 7, [(0, (30, 70, 90)), (2880, (30, 70, 90))]),   # underwater fog (global)
    int_band(3, 16, [(1440, (120, 160, 60))]),                   # zone light over 33_48: greenish river
    int_band(3, 17, [(1440, (40, 60, 20))]),
    int_band(4, 7, [(1440, (50, 80, 30))]),
]
write("DBFilesClient/LightIntBand.dbc", dbc(34, ib))
write("DBFilesClient/LightFloatBand.dbc", dbc(34, [float_band(2, 0, [(1440, 55 * 36)]), float_band(4, 0, [(1440, 30 * 36)])]))
exp.append("L 32 48 0 90 150 170 20 50 70 30 70 90 55")
exp.append("L 33 48 0 120 160 60 40 60 20 50 80 30 30")
exp.append("F 0 30")
exp.append("F 1 5")
exp.append("F 2 0")

# ------------------------------------------------------------ WMO with liquids
def mogp_group(lo, hi, flags, liquid_type, mliq):
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    V = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)]
    I = [0, 1, 2, 0, 2, 3]
    sub = chunk("MOPY", b"".join(struct.pack("<BB", 0x20, 0) for _ in range(2)))
    sub += chunk("MOVI", struct.pack("<6H", *I))
    sub += chunk("MOVT", b"".join(struct.pack("<fff", *v) for v in V))
    sub += chunk("MONR", b"".join(struct.pack("<fff", 0, 0, 1) for _ in V))
    sub += chunk("MOTV", b"".join(struct.pack("<ff", 0, 0) for _ in V))
    sub += chunk("MOBA", struct.pack("<6hIHHHBB", 0, 0, 0, 0, 0, 0, 0, 6, 0, 3, 0, 0))
    sub += chunk("MLIQ", mliq)
    mogp = struct.pack("<III", 0, 0, flags) + struct.pack("<6f", *lo, *hi) + struct.pack("<HHHHHH", 0, 0, 0, 0, 1, 0)
    mogp += bytes(4) + struct.pack("<III", liquid_type, 0, 0) + bytes(4)
    assert len(mogp) == 68
    return chunk("MVER", struct.pack("<I", 17)) + chunk("MOGP", mogp + sub)

def mliq(xt, yt, base, heights, tiles):
    xv, yv = xt + 1, yt + 1
    d = struct.pack("<IIIIfffH", xv, yv, xt, yt, *base, 0)
    d += b"".join(struct.pack("<If", 0, heights(i, j)) for j in range(yv) for i in range(xv))
    d += bytes(tiles)
    return d

pool_base, pool_tiles = (-6.0, -4.0, 1.0), [0, 0, 0, 0, 0, 0x0F]          # 3 x 2, cell (2, 1) dry
lava_base, lava_tiles = (10.0, 0.0, 0.5), [0x06]
groups = [((-8, -6, 0), (8, 6, 3), 0x8), ((9, -1, 0), (15, 5, 2), 0x8)]
mogi = b"".join(struct.pack("<I6fi", fl, *lo, *hi, -1) for lo, hi, fl in groups)
mohd = struct.pack("<IIIIIIIII6fHH", 1, 2, 0, 0, 0, 0, 1, 0, 777, -8, -6, 0, 15, 6, 3, 0, 0)
momt = struct.pack("<16I", *([0] * 16))
write("World/wmo/Test/Pool.wmo", chunk("MVER", struct.pack("<I", 17)) + chunk("MOHD", mohd) +
      chunk("MOTX", b"Textures\\Test\\Wall.blp\0") + chunk("MOMT", momt) + chunk("MOGN", b"\0\0") + chunk("MOGI", mogi) +
      chunk("MODN", b"") + chunk("MODD", b"") + chunk("MODS", b"Set_$DefaultGlobal".ljust(20, b"\0") + struct.pack("<III", 0, 0, 0)))
write("World/wmo/Test/Pool_000.wmo", mogp_group(*groups[0][:2], 0x8, 0, mliq(3, 2, pool_base, lambda i, j: 1.0, pool_tiles)))
write("World/wmo/Test/Pool_001.wmo", mogp_group(*groups[1][:2], 0x8, 2, mliq(1, 1, lava_base, lambda i, j: 0.5, lava_tiles)))

def render_to_adt(x, y, z): return (ZP - y, z, ZP - x)
def modf(name_id, uid, render_pos, rot_deg):
    ax, ay, az = render_to_adt(*render_pos)
    return struct.pack("<II3f3f3f3fHHHH", name_id, uid, ax, ay, az, *rot_deg, ax - 40, ay - 5, az - 40, ax + 40, ay + 25, az + 40, 0, 0, 0, 0)

placements = [((-8800.0, -100.0, 80.0), 30.0, 901), ((-8800.0, -536.0, 80.0), 0.0, 902)]
modfs = [modf(0, uid, pos, (0.0, ang - 180.0, 0.0)) for pos, ang, uid in placements]

# an instance map (stage 5.24 path): the same building as the map's global WMO, MODF at the middle of the map
# (world (0, 0, 100); MODF rotation 0 = 180 degrees about z in render space)
gmodf = struct.pack("<II3f3f6fHHHH", 0, 4321, ZP, 100.0, ZP, 0.0, 0.0, 0.0, -50, -50, 90, 50, 50, 120, 0, 0, 0, 0)
write("World/Maps/Grotto/Grotto.wdt", chunk("MVER", struct.pack("<I", 18)) + chunk("MPHD", struct.pack("<8I", 1, 0, 0, 0, 0, 0, 0, 0)) +
      chunk("MAIN", bytes(64 * 64 * 8)) + chunk("MWMO", b"World\\wmo\\Test\\Pool.wmo\0") + chunk("MODF", gmodf))

# building cells in render space: [(corners, kind)]
def building_cells(placements):
  quads = []
  for pos, ang, uid in placements:
    a = math.radians(ang)
    def W(p, pos=pos, a=a):
        return (pos[0] + math.cos(a) * p[0] - math.sin(a) * p[1], pos[1] + math.sin(a) * p[0] + math.cos(a) * p[1], pos[2] + p[2])
    for base, xt, yt, tiles, kind in ((pool_base, 3, 2, pool_tiles, 0), (lava_base, 1, 1, lava_tiles, 2)):
        for ty in range(yt):
            for tx in range(xt):
                if tiles[ty * xt + tx] & 0x0F == 0x0F: continue
                c = [W((base[0] + (tx + dx) * CELL, base[1] + (ty + dy) * CELL, base[2])) for dx, dy in ((0, 0), (1, 0), (1, 1), (0, 1))]
                quads.append((c, kind))
  return quads
quads = building_cells(placements)

def tile_of(x, y): return (int(math.floor(32 - y / TS)), int(math.floor(32 - x / TS)))
def tile_max(tx, ty): return ((32 - ty) * TS, (32 - tx) * TS)

def inside(px, py, c):
    def tri(a, b, d):
        den = (b[1] - d[1]) * (a[0] - d[0]) + (d[0] - b[0]) * (a[1] - d[1])
        if abs(den) < 1e-9: return False
        w0 = ((b[1] - d[1]) * (px - d[0]) + (d[0] - b[0]) * (py - d[1])) / den
        w1 = ((d[1] - a[1]) * (px - d[0]) + (a[0] - d[0]) * (py - d[1])) / den
        return w0 >= -1e-4 and w1 >= -1e-4 and 1 - w0 - w1 >= -1e-4
    return tri(c[0], c[1], c[2]) or tri(c[0], c[2], c[3])

# ------------------------------------------------------------ ADTs
def mclq_layer(heights, tiles, depth=0):
    d = struct.pack("<ff", min(heights), max(heights))
    d += b"".join(struct.pack("<BBBBf", depth, 0, 0, 0, h) for h in heights)
    d += bytes(tiles) + bytes(84)
    assert len(d) == 804
    return d

def adt(tx, ty, liquids, terrain_z, modf_list):
    """liquids: {(cy, cx): (mcnk liquid flags, [layer bytes])}; terrain_z: {(cy, cx): z}"""
    mwmo = b"World\\wmo\\Test\\Pool.wmo\0"
    data = chunk("MVER", struct.pack("<I", 18)) + chunk("MTEX", b"Textures\\Test\\Wall.blp\0") + chunk("MMDX", b"") + chunk("MMID", b"")
    data += chunk("MWMO", mwmo) + chunk("MWID", struct.pack("<I", 0)) + chunk("MDDF", b"") + chunk("MODF", b"".join(modf_list))
    for cy in range(16):
        for cx in range(16):
            flags, layers = liquids.get((cy, cx), (0, []))
            hdrc = bytearray(128)
            struct.pack_into("<IIII", hdrc, 0, flags, cx, cy, 0)
            struct.pack_into("<II", hdrc, 20, 136, 136 + 588)               # ofsHeight, ofsNormal: from the MCNK start
            struct.pack_into("<fff", hdrc, 104, 0, 0, terrain_z.get((cy, cx), 80.0))
            body = chunk("MCVT", struct.pack("<145f", *([0.0] * 145))) + chunk("MCNR", bytes([0, 0, 127] * 145) + bytes(13))
            if layers:
                ofs = 8 + 128 + len(body)
                payload = b"".join(layers)
                struct.pack_into("<II", hdrc, 0x60, ofs, 8 + len(payload))
                body += b"QLCM" + struct.pack("<I", 0) + payload          # vanilla: the MCLQ header's own size is 0
            data += chunk("MCNK", bytes(hdrc) + body)
    write(f"World/Maps/Test/Test_{tx}_{ty}.adt", data)

full = [0] * 64
lake_tiles = [0x0F] + [0] * 63
two_river = [0 if k % 8 <= 3 else 0x0F for k in range(64)]
two_magma = [0 if k % 8 >= 2 else 0x0F for k in range(64)]
slope_h = [78 - 0.5 * (v // 9) for v in range(81)]
liq32 = {
    (2, 3): (0x04, [mclq_layer([78.0] * 81, lake_tiles)]),
    (2, 4): (0x04 | 0x10, [mclq_layer([78.0] * 81, two_river), mclq_layer([79.5] * 81, two_magma)]),
    (5, 5): (0x08, [mclq_layer([0.0] * 81, [0x81] * 64)]),
    (6, 6): (0x04, [mclq_layer(slope_h, full)]),
    (7, 7): (0x04, [mclq_layer([78.0] * 81, [0x0F] * 64)]),
}
terr32 = {(2, 3): 70.0, (2, 4): 70.0, (5, 5): -20.0, (6, 6): 70.0}
adt(32, 48, liq32, terr32, modfs)
liq33 = {(1, 1): (0x04, [mclq_layer([60.0] * 81, full)])}
adt(33, 48, liq33, {(1, 1): 50.0}, modfs)

# ------------------------------------------------------------ expectations
def cell_centre(tx, ty, cy, cx, r, k):
    mx, my = tile_max(tx, ty)
    return mx - (cy * 8 + r + 0.5) * CELL, my - (cx * 8 + k + 0.5) * CELL

def q(x, y, wet, flags=0xFF, level=0.0):
    exp.append(f"Q {x:.4f} {y:.4f} {1 if wet else 0} {flags} {level:.3f}")

x, y = cell_centre(32, 48, 2, 3, 3, 3); q(x, y, True, 0, 78.0)
x, y = cell_centre(32, 48, 2, 3, 0, 0); q(x, y, False)
x, y = cell_centre(32, 48, 2, 4, 3, 1); q(x, y, True, 0, 78.0)
x, y = cell_centre(32, 48, 2, 4, 3, 3); q(x, y, True, 2, 79.5)
x, y = cell_centre(32, 48, 2, 4, 5, 6); q(x, y, True, 2, 79.5)
x, y = cell_centre(32, 48, 5, 5, 4, 4); q(x, y, True, 1 | 4, 0.0)
x, y = cell_centre(32, 48, 6, 6, 2, 3); q(x, y, True, 0, 78 - 0.5 * 2.5)
x, y = cell_centre(32, 48, 6, 6, 5, 0); q(x, y, True, 0, 78 - 0.5 * 5.5)
x, y = cell_centre(32, 48, 7, 7, 4, 4); q(x, y, False)
x, y = cell_centre(32, 48, 0, 0, 4, 4); q(x, y, False)
x, y = cell_centre(33, 48, 1, 1, 2, 2); q(x, y, True, 0, 60.0)

# building cells: every cell centre of the grid near the buildings, rasterised the same way as the converter
def raster(quads, out):
  owned = {}
  for c, kind in quads:
    mxq = sum(p[0] for p in c) / 4; myq = sum(p[1] for p in c) / 4
    owned.setdefault(tile_of(mxq, myq), []).append((c, kind))
  wet = 0
  for (tx, ty), lst in owned.items():
    mx, my = tile_max(tx, ty)
    xs = [p[0] for c, _ in lst for p in c]; ys = [p[1] for c, _ in lst for p in c]
    gx0 = max(0, int(math.floor((mx - max(xs)) / CELL)) - 1); gx1 = min(127, int(math.floor((mx - min(xs)) / CELL)) + 1)
    gy0 = max(0, int(math.floor((my - max(ys)) / CELL)) - 1); gy1 = min(127, int(math.floor((my - min(ys)) / CELL)) + 1)
    for gx in range(gx0, gx1 + 1):
        for gy in range(gy0, gy1 + 1):
            px, py = mx - (gx + 0.5) * CELL, my - (gy + 0.5) * CELL
            hit = [(c, k) for c, k in lst if inside(px, py, c)]
            if hit:
                c, k = hit[0]
                out.append(f"Q {px:.4f} {py:.4f} 1 {k | 8} {c[0][2]:.3f}"); wet += 1
            else:
                out.append(f"Q {px:.4f} {py:.4f} 0 255 0")
  return wet
wmo_wet = raster(quads, exp)
assert wmo_wet >= 8, wmo_wet
dexp = []
dungeon_wet = raster(building_cells([((0.0, 0.0, 100.0), 180.0, 0)]), dexp)
assert dungeon_wet >= 4, dungeon_wet
open(expect_path.replace(".txt", "_dungeon.txt"), "w").write("\n".join(dexp) + "\n")
# merged quads: the deep, flat ocean chunk is one rectangle
exp.append("T 32 48 1 2")
open(expect_path, "w").write("\n".join(exp) + "\n")
print(f"water data: {len(exp)} expectations, {len(quads)} building cells, {wmo_wet} wet grid cells from buildings")
