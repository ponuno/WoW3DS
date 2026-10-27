"""Synthetic ship for w3dconv --transports (stage 5.21).

    python3 gen_transport_data.py <Data dir> <expect.json>

Adds to a Data/ tree:
  World/Generic/Human/Passive Doodads/Ship/Testship.m2   hull box + a mast box, collision mesh: a flat deck at z = 5.5
                                                         (x -8..8, y -3..3) and a cabin roof at z = 9 (x -2..2, y -1..1)
  DBFilesClient/GameObjectDisplayInfo.dbc                3015 = the ship, 3016 = a chest (no transport keyword)
  DBFilesClient/TaxiPathNode.dbc                         path 9 (a ferry, stops at both ends) and path 10 (a flight)
"""
import json, os, struct, sys
from m2_writer import Blob, blp, m2
out, exp_path = sys.argv[1], sys.argv[2]

def write(path, data):
    p = os.path.join(out, path.replace("\\", "/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data)

def box(cx, cy, cz, sx, sy, sz):
    V = []
    for k in range(8):
        p = (cx + (sx if k & 1 else -sx), cy + (sy if k & 2 else -sy), cz + (sz if k & 4 else -sz))
        n = (1 if k & 1 else -1, 1 if k & 2 else -1, 1 if k & 4 else -1)
        V.append((p, (255, 0, 0, 0), (0, 0, 0, 0), n, ((k & 1) * 1.0, (k >> 1 & 1) * 1.0)))
    T = [0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5]
    return V, T

verts, tris = [], []
for b in (box(0, 0, 3, 8, 3, 2.5), box(0, 0, 8, 0.3, 0.3, 4)):
    base = len(verts); verts += b[0]; tris += [base + i for i in b[1]]
nt = len(tris)
# collision: deck quad (2 triangles, up-facing) and a smaller roof quad
cv = [(-8, -3, 5.5), (8, -3, 5.5), (8, 3, 5.5), (-8, 3, 5.5), (-2, -1, 9), (2, -1, 9), (2, 1, 9), (-2, 1, 9)]
ci = [0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7]
ship = m2("Testship", verts, tris, [(0, 0, nt)], [(0, 0, 0)], [(0, "World\\Generic\\Human\\Passive Doodads\\Ship\\Testship.blp")],
          [0], [(0, 0)], [(-1, (0, 0, 0), {})], [(0, 0, 0, 1000, 0.0)], [], coll=(cv, ci))
write("World/Generic/Human/Passive Doodads/Ship/Testship.m2", ship)
write("World/Generic/Human/Passive Doodads/Ship/Testship.blp", blp(4))

def dbc(fields, rows, strings):
    recs = b"".join(struct.pack("<" + "".join("i" if isinstance(v, int) and v < 0 else ("f" if isinstance(v, float) else "I") for v in r), *r) for r in rows)
    return b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, len(strings)) + recs + strings

strings = b"\0"
def sref(t):
    global strings
    off = len(strings); strings += t.encode() + b"\0"; return off
rows = []
for did, name in ((3015, "World\\Generic\\Human\\Passive Doodads\\Ship\\Testship.mdx"), (3016, "World\\Generic\\Human\\Passive Doodads\\Chest\\Chest01.mdx")):
    rows.append([did, sref(name)] + [0] * 10 + [0.0] * 6)
os.makedirs(os.path.join(out, "DBFilesClient"), exist_ok=True)
open(os.path.join(out, "DBFilesClient", "GameObjectDisplayInfo.dbc"), "wb").write(dbc(18, rows, strings))

def node(i, path, idx, x, y, z, flags=0, delay=0, mapid=0):
    return [i, path, idx, mapid, float(x), float(y), float(z), flags, delay]
nodes, i = [], 1
nodes.append(node(i, 9, 0, -20, 0, 5)); i += 1
nodes.append(node(i, 9, 1, 0, 0, 5, 2, 10)); i += 1
for x in range(50, 600, 50):
    nodes.append(node(i, 9, 1 + x // 50, x, 0, 5)); i += 1
nodes.append(node(i, 9, 13, 600, 0, 5, 2, 10)); i += 1
nodes.append(node(i, 9, 14, -5, 0, 5)); i += 1
for k in range(4):
    nodes.append(node(i, 10, k, 100 * k, 0, 50)); i += 1
open(os.path.join(out, "DBFilesClient", "TaxiPathNode.dbc"), "wb").write(dbc(9, nodes, b"\0"))
json.dump({"deckZ": 5.5, "x0": -8, "y0": -3, "x1": 8, "y1": 3, "display": 3015, "pathNodes": len(nodes)}, open(exp_path, "w"))
