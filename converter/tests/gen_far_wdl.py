"""The map's WDL for the far-ground test (6.24): World/Maps/Test/Test.wdl next to gen_terrain_layers.py's Test_32_48.adt.

    python3 gen_far_wdl.py <out Data dir>

Tiles (MAOF index [tileY * 64 + tileX], outer heights [i * 17 + j] as the client reads them):
    32_48  the converted tile: the rough ground's chunk corners rounded to whole yards (lets the converter tell the four
           possible readings apart - the right one agrees to within 0.5 yd, the others are off by yards)
    33_48  and 32_49: h = 100 + 2 i + j (not converted: the far ground takes them from here)
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import terrain_rough as rough

out = sys.argv[1]

def chunk(magic, payload):
    return magic[::-1].encode() + struct.pack("<I", len(payload)) + payload

tiles = {}
tiles[(32, 48)] = [int(round(rough.BASE + rough.height(8 * i, 8 * j))) for i in range(17) for j in range(17)]
for t in [(33, 48), (32, 49)]:
    tiles[t] = [100 + 2 * i + j for i in range(17) for j in range(17)]

head = chunk("MVER", struct.pack("<I", 18))
maof_pos = len(head)
offs = [0] * 4096
body = b""
base = maof_pos + 8 + 4096 * 4
for (tx, ty), h in sorted(tiles.items()):
    offs[ty * 64 + tx] = base + len(body)
    body += chunk("MARE", struct.pack("<289h", *h) + struct.pack("<256h", *([0] * 256)))
    body += chunk("MAHO", bytes(32))
data = head + chunk("MAOF", struct.pack("<4096I", *offs)) + body
p = os.path.join(out, "World/Maps/Test/Test.wdl")
os.makedirs(os.path.dirname(p), exist_ok=True)
open(p, "wb").write(data)
