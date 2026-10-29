"""Synthetic world objects for w3dconv --creatures --transports (stage 5.41).

    python3 gen_worldobj_data.py <Data dir> <expect.json>

Adds to a Data/ tree:
  World/Generic/Undercity/Elevator/Testelevator.m2   a platform box, collision: a flat deck at z = 0.4 (x -3..3, y -3..3)
  World/Generic/ActiveDoodads/Doors/Testdoor.m2      a thin leaf; sequences stand(0), close(146), closed(147), open(148), opened(149)
  World/Generic/ActiveDoodads/Mailbox/Testmailbox.m2 a box (default set: "mailbox")
  World/Generic/Rocks/Testboulder.m2                 a box (matches nothing: must stay out)
  DBFilesClient/GameObjectDisplayInfo.dbc            4001 elevator, 4002 door, 4003 mailbox, 4004 boulder
  DBFilesClient/TransportAnimation.dbc               entry 20650: 3 key points, entry 20651: 2 key points (no TaxiPathNode.dbc at all)
"""
import json, os, struct, sys
from m2_writer import blp, m2
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

def model(name, path, b, seqs, coll=None):
    v, t = b
    m = m2(name, v, t, [(0, 0, len(t))], [(0, 0, 0)], [(0, path + ".blp")], [0], [(0, 0)], [(-1, (0, 0, 0), {})], seqs, [], coll=coll)
    write(path + ".m2", m)
    write(path + ".blp", blp(4))

ELEV = "World\\Generic\\Undercity\\Elevator\\Testelevator"
DOOR = "World\\Generic\\ActiveDoodads\\Doors\\Testdoor"
MAIL = "World\\Generic\\ActiveDoodads\\Mailbox\\Testmailbox"
ROCK = "World\\Generic\\Rocks\\Testboulder"
cv = [(-3, -3, 0.4), (3, -3, 0.4), (3, 3, 0.4), (-3, 3, 0.4)]
model("Testelevator", ELEV, box(0, 0, 0, 3, 3, 0.4), [(0, 0, 0, 1000, 0.0)], coll=(cv, [0, 1, 2, 0, 2, 3]))
model("Testdoor", DOOR, box(0, 0, 2, 0.15, 1.5, 2), [(0, 0, 0, 1000, 0.0), (146, 0, 0, 800, 0.0), (147, 0, 0, 500, 0.0), (148, 0, 0, 800, 0.0),
                                                  (149, 0, 0, 500, 0.0)])
model("Testmailbox", MAIL, box(0, 0, 0.5, 0.3, 0.3, 0.5), [(0, 0, 0, 1000, 0.0)])
model("Testboulder", ROCK, box(0, 0, 1, 1, 1, 1), [(0, 0, 0, 1000, 0.0)])

# 6.14: the deck rules. A tram car whose collision is missing (the drawn floor is used), a bridge with a flat collision floor (a
# platform among the objects: S line), a lamp post (collision far too small: nothing) and a flat drawn thing without any collision
# mesh 1 yd up (a ground glow / rug: it is not solid, so no S line).
TRAM = "World\\Generic\\Gnome\\Passivedoodads\\Subway\\Testtram"
BRIDGE = "World\\Generic\\Human\\Passivedoodads\\Bridge\\Testbridge"
POST = "World\\Generic\\Human\\Passivedoodads\\Lamps\\Testpost"
RUG = "World\\Generic\\Human\\Passivedoodads\\Rugs\\Testrug"
def quad(x0, y0, x1, y1, z):
    V = [((x, y, z), (255, 0, 0, 0), (0, 0, 0, 0), (0, 0, 1), (u, v)) for (x, y, u, v) in ((x0, y0, 0.0, 0.0), (x1, y0, 1.0, 0.0), (x1, y1, 1.0, 1.0), (x0, y1, 0.0, 1.0))]
    return V, [0, 1, 2, 0, 2, 3]
model("Testtram", TRAM, quad(-3.0, -1.25, 3.0, 1.25, 0.3), [(0, 0, 0, 1000, 0.0)])
model("Testbridge", BRIDGE, box(0, 0, 0.5, 4, 1.5, 0.5), [(0, 0, 0, 1000, 0.0)],
      coll=([(-4, -1.5, 1.0), (4, -1.5, 1.0), (4, 1.5, 1.0), (-4, 1.5, 1.0)], [0, 1, 2, 0, 2, 3]))
model("Testpost", POST, box(0, 0, 1, 0.3, 0.3, 1), [(0, 0, 0, 1000, 0.0)],
      coll=([(-0.5, -0.5, 0.5), (0.5, -0.5, 0.5), (0.5, 0.5, 0.5), (-0.5, 0.5, 0.5)], [0, 1, 2, 0, 2, 3]))
model("Testrug", RUG, quad(-4.0, -1.5, 4.0, 1.5, 1.0), [(0, 0, 0, 1000, 0.0)])

def dbc(fields, rows, strings):
    recs = b"".join(struct.pack("<" + "".join("i" if isinstance(v, int) and v < 0 else ("f" if isinstance(v, float) else "I") for v in r), *r) for r in rows)
    return b"WDBC" + struct.pack("<IIII", len(rows), fields, fields * 4, len(strings)) + recs + strings

strings = b"\0"
def sref(t):
    global strings
    off = len(strings); strings += t.encode() + b"\0"; return off
rows = []
for did, name in ((4001, ELEV), (4002, DOOR), (4003, MAIL), (4004, ROCK), (4005, TRAM), (4006, BRIDGE), (4007, POST), (4008, RUG)):
    rows.append([did, sref(name + ".mdx")] + [0] * 10 + [0.0] * 6)
os.makedirs(os.path.join(out, "DBFilesClient"), exist_ok=True)
open(os.path.join(out, "DBFilesClient", "GameObjectDisplayInfo.dbc"), "wb").write(dbc(18, rows, strings))

anim = [[1, 20650, 0, 0.0, 0.0, 0.0, 0], [2, 20650, 4000, 0.0, 0.0, 0.0, 0], [3, 20650, 10000, 2.0, 0.0, -40.0, 0],
        [4, 20651, 0, 0.0, 0.0, 0.0, 0], [5, 20651, 8000, 0.0, 5.0, 0.0, 0]]
open(os.path.join(out, "DBFilesClient", "TransportAnimation.dbc"), "wb").write(dbc(7, anim, b"\0"))
json.dump({"elevator": 4001, "door": 4002, "mailbox": 4003, "boulder": 4004, "tram": 4005, "bridge": 4006, "post": 4007, "rug": 4008,
           "deckZ": 0.4, "tramZ": 0.3, "bridgeZ": 1.0, "animRows": 5, "animEntries": 2,
           "doorSeqs": [0, 146, 147, 148, 149]}, open(exp_path, "w"))
