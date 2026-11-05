"""6.59c: a building whose waters pass the 65535 vertices of one tile's water mesh (Turtle WoW's custom maps lay big, stacked building
waters: 400 000 vertices in a tile). Lake.wmo: two groups with the same 100 x 90 MLIQ cells (9000 each, flat at local z 1 and 2:
18000 quads, 72000 vertices drawn one by one), placed unturned in tile 32_48 of map Test (no MCLQ). The converter must draw the
tile's water from the 4-yd query grid instead (the upper surface, merged) and say so.
  gen_bigwater_data.py <data dir>"""
import os, struct, sys
TS = 533.33333
ZP = 32 * TS
CELL = TS / 128
out = sys.argv[1]
def chunk(m, p): return m[::-1].encode() + struct.pack("<I", len(p)) + p
def write(path, data):
    p = os.path.join(out, path.replace("\\", "/")); os.makedirs(os.path.dirname(p), exist_ok=True); open(p, "wb").write(data)
def dbc(fields, rows, strings=b"\0"):
    recs = b"".join(b"".join(struct.pack("<I", v) for v in r) for r in rows)
    return b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, len(strings)) + recs + strings
write("DBFilesClient/Map.dbc", dbc(3, [[0, 1, 0]], b"\0Test\0"))
pal = b"".join(struct.pack("<BBBB", 100 + i // 3, 100 + i // 3, 100 + i // 3, 255) for i in range(256))
for f in range(1, 31):
    pix = bytes((x * 5 + y * 3 + f * 8) % 256 for y in range(32) for x in range(32))
    hdr = b"BLP2" + struct.pack("<IBBBBII", 1, 1, 0, 0, 0, 32, 32)
    offs = [len(hdr) + 64 + 64 + 1024] + [0] * 15; sizes = [len(pix)] + [0] * 15
    write(f"XTextures/river/lake_a.{f}.blp", hdr + struct.pack("<16I", *offs) + struct.pack("<16I", *sizes) + pal + pix)
XT, YT = 100, 90
base = (-208.0, -187.0)
def group(z):
    lo, hi = (base[0], base[1], 0.0), (base[0] + XT * CELL, base[1] + YT * CELL, 3.0)
    V = [(lo[0], lo[1], 0), (hi[0], lo[1], 0), (hi[0], hi[1], 0), (lo[0], hi[1], 0)]
    sub = chunk("MOPY", struct.pack("<BB", 0x20, 0) * 2) + chunk("MOVI", struct.pack("<6H", 0, 1, 2, 0, 2, 3))
    sub += chunk("MOVT", b"".join(struct.pack("<fff", *v) for v in V)) + chunk("MONR", struct.pack("<fff", 0, 0, 1) * 4)
    sub += chunk("MOTV", struct.pack("<ff", 0, 0) * 4) + chunk("MOBA", struct.pack("<6hIHHHBB", 0, 0, 0, 0, 0, 0, 0, 6, 0, 3, 0, 0))
    m = struct.pack("<IIIIfffH", XT + 1, YT + 1, XT, YT, base[0], base[1], z, 0)
    m += struct.pack("<If", 0, z) * ((XT + 1) * (YT + 1)) + bytes(XT * YT)
    sub += chunk("MLIQ", m)
    mogp = struct.pack("<III", 0, 0, 0x8) + struct.pack("<6f", *lo, *hi) + struct.pack("<HHHHHH", 0, 0, 0, 0, 1, 0)
    mogp += bytes(4) + struct.pack("<III", 0, 0, 0) + bytes(4)
    return chunk("MVER", struct.pack("<I", 17)) + chunk("MOGP", mogp + sub), (lo, hi)
g0, b0 = group(1.0)
g1, b1 = group(2.0)
mogi = b"".join(struct.pack("<I6fi", 0x8, *lo, *hi, -1) for lo, hi in (b0, b1))
mohd = struct.pack("<IIIIIIIII6fHH", 1, 2, 0, 0, 0, 0, 1, 0, 778, *b0[0], *b0[1], 0, 0)
write("World/wmo/Test/Lake.wmo", chunk("MVER", struct.pack("<I", 17)) + chunk("MOHD", mohd) + chunk("MOTX", b"Textures\\Test\\Wall.blp\0") +
      chunk("MOMT", bytes(64)) + chunk("MOGN", b"\0\0") + chunk("MOGI", mogi) + chunk("MODN", b"") + chunk("MODD", b"") +
      chunk("MODS", b"Set_$DefaultGlobal".ljust(20, b"\0") + struct.pack("<III", 0, 0, 0)))
write("World/wmo/Test/Lake_000.wmo", g0)
write("World/wmo/Test/Lake_001.wmo", g1)
pos = (-8800.0, -266.0, 80.0)
ax, ay, az = ZP - pos[1], pos[2], ZP - pos[0]
modf = struct.pack("<II3f3f3f3fHHHH", 0, 990, ax, ay, az, 0.0, -180.0, 0.0, ax - 250, ay - 5, az - 250, ax + 250, ay + 25, az + 250, 0, 0, 0, 0)
data = chunk("MVER", struct.pack("<I", 18)) + chunk("MTEX", b"Textures\\Test\\Wall.blp\0") + chunk("MMDX", b"") + chunk("MMID", b"")
data += chunk("MWMO", b"World\\wmo\\Test\\Lake.wmo\0") + chunk("MWID", struct.pack("<I", 0)) + chunk("MDDF", b"") + chunk("MODF", modf)
for cy in range(16):
    for cx in range(16):
        h = bytearray(128)
        struct.pack_into("<IIII", h, 0, 0, cx, cy, 0)
        struct.pack_into("<II", h, 20, 136, 136 + 588)
        struct.pack_into("<fff", h, 104, 0, 0, 80.0)
        body = chunk("MCVT", struct.pack("<145f", *([0.0] * 145))) + chunk("MCNR", bytes([0, 0, 127] * 145) + bytes(13))
        data += chunk("MCNK", bytes(h) + body)
write("World/Maps/Test/Test_32_48.adt", data)
