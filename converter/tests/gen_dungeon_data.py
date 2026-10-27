"""Adds an instance map to the Data tree of gen_wmo_data.py (no client files needed).

    python3 gen_dungeon_data.py <Data dir>

World/Maps/Crypt/Crypt.wdt: MPHD flag 1 (global WMO), MWMO = the House building, MODF at the middle of the map,
DBFilesClient/Map.dbc: id 36 "Crypt" (a dungeon), id 0 "Test" (an ordinary map: must be skipped by --dungeons).
"""
import os, struct, sys

d = sys.argv[1]
ZP = 32 * 533.33333

def chunk(magic, payload):
    return magic[::-1].encode() + struct.pack("<I", len(payload)) + payload

def write(path, data):
    p = os.path.join(d, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data)

modf = struct.pack("<II3f3f6fHHHH", 0, 1234, ZP, 100.0, ZP, 0.0, 0.0, 0.0, -50, -50, 90, 50, 50, 120, 0, 0, 0, 0)
assert len(modf) == 64
wdt = (chunk("MVER", struct.pack("<I", 18)) + chunk("MPHD", struct.pack("<8I", 1, 0, 0, 0, 0, 0, 0, 0)) +
       chunk("MAIN", bytes(64 * 64 * 8)) + chunk("MWMO", b"World\\wmo\\Test\\House.wmo\0") + chunk("MODF", modf))
write("World/Maps/Crypt/Crypt.wdt", wdt)

# Map.dbc: 40 fields like the real one (field 1 = directory, field 38 = LoadingScreens id)
names = ["", "Crypt", "Test", "Kalimdor", "Azeroth", "Outland", "Other"]
strs = b"".join(n.encode() + b"\0" for n in names)
offs = {}; pos = 0
for n in names:
    offs[n] = pos; pos += len(n) + 1
rows = [(36, "Crypt", 5), (0, "Test", 3), (1, "Kalimdor", 4), (13, "Azeroth", 2), (30, "Outland", 6), (33, "Other", 7)]
recs = b""
for mid, dirn, ls in rows:
    f = [0] * 40
    f[0] = mid; f[1] = offs[dirn]; f[13] = 1 + mid % 3; f[38] = ls   # f[13]: a column with few distinct values (min level)
    recs += struct.pack("<40I", *f)
write("DBFilesClient/Map.dbc", b"WDBC" + struct.pack("<IIII", len(rows), 40, 160, len(strs)) + recs + strs)

# LoadingScreens.dbc: id, name, path; the pictures are 128x128 palettised BLP2 (square: the picture is the top 3/4)
paths = {i: "Interface\\Glues\\LoadingScreens\\LoadScreen%d.blp" % i for i in range(2, 8)}
lstr = b"\0"; lrecs = b""
for i, pth in paths.items():
    o = len(lstr); lstr += pth.encode() + b"\0"
    lrecs += struct.pack("<III", i, 0, o)
    W = H = 128
    pal = b"".join(struct.pack("<BBBB", (i * 30 + k) % 256, (k * 2) % 256, 255 - k, 255) for k in range(256))
    pix = bytes((x + y + i * 20) % 256 for y in range(H) for x in range(W))
    hdr = b"BLP2" + struct.pack("<IBBBBII", 1, 1, 0, 0, 0, W, H)
    offs2 = [0] * 16; sizes = [0] * 16
    offs2[0] = len(hdr) + 64 + 64 + 1024; sizes[0] = len(pix)
    write(pth.replace("\\", "/"), hdr + struct.pack("<16I", *offs2) + struct.pack("<16I", *sizes) + pal + pix)
write("DBFilesClient/LoadingScreens.dbc", b"WDBC" + struct.pack("<IIII", len(paths), 3, 12, len(lstr)) + lrecs + lstr)
