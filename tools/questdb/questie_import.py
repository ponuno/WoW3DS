#!/usr/bin/env python3
"""6.63: Questie's database (QuestieDB, data/Classic/*.lua) -> questdb.bin for the 3DS client (quest areas on the map).

    questie_import.py <QuestieDB/data/Classic> <out questdb.bin>

Per quest: where it is given / handed in (spawn points of the NPCs / objects) and, for each objective, blobs (zone, centre, radius,
all in zone percent x 100) covering the spawns of the creatures / objects it asks for, or of whatever drops the item it asks for
(Questie's own data: npcDrops / objectDrops). The client turns (zone, x, y) into world coordinates with the WorldMapArea bounds of
ui.w3dp. Questie is GPL-3.

File (little endian):
  header   'WQDB', u32 version 1, u32 count, u32 offIndex
  index    count x {u32 questId, u32 offset}   sorted by id
  record   u8 nStart, u8 nEnd, u8 nObj, u8 0, then nStart + nEnd points {u16 area, u16 x, u16 y} (x, y: percent x 100)
           then per objective: u8 kind (0 kill, 1 object, 2 item, 3 explore), u8 nBlobs, u16 0, u32 id (creature / object / item;
           explore: 0), blobs {u16 area, u16 x, u16 y, u16 r}
"""
import struct, sys, re

class P:
    def __init__(s, t): s.t = t; s.i = 0
    def ws(s):
        t = s.t
        while s.i < len(t) and t[s.i] in " \t\r\n": s.i += 1
    def value(s):
        s.ws(); t = s.t; c = t[s.i]
        if c == "{": return s.table()
        if c in "'\"":
            q = c; s.i += 1; out = []
            while t[s.i] != q:
                if t[s.i] == "\\": s.i += 1
                out.append(t[s.i]); s.i += 1
            s.i += 1
            return "".join(out)
        if t.startswith("nil", s.i): s.i += 3; return None
        if t.startswith("true", s.i): s.i += 4; return True
        if t.startswith("false", s.i): s.i += 5; return False
        m = re.compile(r"-?\d+(\.\d+)?([eE][-+]?\d+)?").match(t, s.i)
        s.i = m.end(); v = float(m.group(0))
        return int(v) if v == int(v) and "." not in m.group(0) else v
    def table(s):
        s.i += 1; arr = []; d = {}; n = 0
        while True:
            s.ws()
            if s.t[s.i] == "}": s.i += 1; break
            if s.t[s.i] == "[":
                s.i += 1; k = s.value(); s.ws(); s.i += 1   # ]
                s.ws(); s.i += 1                            # =
                d[k] = s.value()
            else:
                arr.append(s.value())
            s.ws()
            if s.t[s.i] in ",;": s.i += 1
        return (d, arr)

def load(path, name):
    txt = open(path, encoding="utf-8").read()
    m = re.search(r"QuestieDB\." + name + r" = \[\[return ", txt)
    body = txt[m.end():]
    end = body.rindex("}]]")
    p = P(body[:end + 1])
    d, _ = p.table()
    return d

def seq(t):   # a Lua array with nils: (dict, list) -> list; nil entries come out as None in the list
    return t[1] if isinstance(t, tuple) else []

def spawns_of(row, idx):   # {zone: [[x, y], ...]}
    t = row[idx] if idx < len(row) else None
    out = {}
    if isinstance(t, tuple):
        for z, pts in t[0].items():
            out[z] = [(p[1][0], p[1][1]) for p in pts[1] if isinstance(p, tuple) and len(p[1]) >= 2 and 0 <= p[1][0] <= 100 and 0 <= p[1][1] <= 100] if isinstance(pts, tuple) else []
    return out

def row_list(t):   # row table -> positional list (1-based into python list index 0)
    return t[1] if isinstance(t, tuple) else []

def blobs(points_by_zone, cell=2.5, maxBlobs=24):
    """Greedy grid clustering in zone percent: a blob per occupied cell group, merged to at most maxBlobs."""
    out = []
    for z, pts in points_by_zone.items():
        cells = {}
        for x, y in pts:
            cells.setdefault((int(x // cell), int(y // cell)), []).append((x, y))
        # connected components of occupied cells (8-neighbourhood)
        seen = set()
        for c in cells:
            if c in seen: continue
            comp = []; stack = [c]; seen.add(c)
            while stack:
                a = stack.pop(); comp.append(a)
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        b = (a[0] + dx, a[1] + dy)
                        if b in cells and b not in seen: seen.add(b); stack.append(b)
            ps = [p for a in comp for p in cells[a]]
            cx = sum(p[0] for p in ps) / len(ps); cy = sum(p[1] for p in ps) / len(ps)
            r = max(max(((p[0] - cx) ** 2 + (p[1] - cy) ** 2) ** 0.5 for p in ps), 0.8) + 0.6
            out.append([z, cx, cy, min(r, 12.0), len(ps)])
    # too many: merge the nearest pair of the same zone until the limit
    while len(out) > maxBlobs:
        best = None
        for i in range(len(out)):
            for j in range(i + 1, len(out)):
                if out[i][0] != out[j][0]: continue
                d = ((out[i][1] - out[j][1]) ** 2 + (out[i][2] - out[j][2]) ** 2) ** 0.5
                if best is None or d < best[0]: best = (d, i, j)
        if best is None: break
        _, i, j = best; a, b = out[i], out[j]
        n = a[4] + b[4]
        cx = (a[1] * a[4] + b[1] * b[4]) / n; cy = (a[2] * a[4] + b[2] * b[4]) / n
        r = max(best[0] / 2 + max(a[3], b[3]), a[3], b[3])
        out[i] = [a[0], cx, cy, min(r, 14.0), n]; del out[j]
    return out

def dots(points_by_zone, maxPts=60):
    """6.76: the spawns themselves, thinned on a grid (one dot per occupied cell, at the cell's mean) and small - a quest area follows
    where the mobs / objects really are (a road, a shore, two camps) and not one big disc over a whole group. The grid coarsens
    (1.0, 1.5, 2.2 ... percent of the zone) until at most maxPts dots are left; a dot's radius covers its cell, so a dense camp still
    reads as one shape. Returns [zone, x, y, r, n] like blobs()."""
    cell = 1.0
    while True:
        out = []
        for z, pts in points_by_zone.items():
            cells = {}
            for x, y in pts:
                cells.setdefault((int(x // cell), int(y // cell)), []).append((x, y))
            for ps in cells.values():
                cx = sum(p[0] for p in ps) / len(ps); cy = sum(p[1] for p in ps) / len(ps)
                out.append([z, cx, cy, min(max(cell * 0.75, 0.7), 5.0), len(ps)])
        if len(out) <= maxPts or cell > 12.0: return out[:maxPts * 2]
        cell *= 1.5

def main():
    src, dst = sys.argv[1], sys.argv[2]
    npcs = load(src + "/classicNpcDB.lua", "npcData")
    objs = load(src + "/classicObjectDB.lua", "objectData")
    items = load(src + "/classicItemDB.lua", "itemData")
    quests = load(src + "/classicQuestDB.lua", "questData")
    def npc_sp(i):
        r = npcs.get(i); return spawns_of(row_list(r), 6) if r else {}
    def obj_sp(i):
        r = objs.get(i); return spawns_of(row_list(r), 3) if r else {}
    def merge(dst, src_):
        for z, pts in src_.items(): dst.setdefault(z, []).extend(pts)
    def item_sp(i):
        r = items.get(i); out = {}
        if not r: return out
        row = row_list(r)
        for k, fn in ((1, npc_sp), (2, obj_sp)):
            t = row[k] if k < len(row) else None
            if isinstance(t, tuple):
                for e in t[1][:60]:   # (a drop from a hundred kinds of mob is no area)
                    if isinstance(e, int): merge(out, fn(e))
        return out
    def flat(points_by_zone, maxPts=3):
        out = []
        for z, pts in points_by_zone.items():
            for p in pts[:maxPts]: out.append((z, p[0], p[1]))
        return out[:6]
    recs = {}
    nobj = nblob = 0
    for qid, q in quests.items():
        row = row_list(q)
        def g(i): return row[i - 1] if i - 1 < len(row) else None
        started, finished, objectives, trig = g(2), g(3), g(10), g(9)
        def ents(t, which):
            tt = row_list(t)
            v = tt[which] if which < len(tt) else None
            return [e for e in row_list(v) if isinstance(e, int)] if isinstance(v, tuple) else []
        start_pts, end_pts = {}, {}
        for e in ents(started, 0): merge(start_pts, npc_sp(e))
        for e in ents(started, 1): merge(start_pts, obj_sp(e))
        for e in ents(finished, 0): merge(end_pts, npc_sp(e))
        for e in ents(finished, 1): merge(end_pts, obj_sp(e))
        objl = []
        ob = row_list(objectives) if isinstance(objectives, tuple) else []
        def entries(slot):
            v = ob[slot] if slot < len(ob) else None
            return [x for x in row_list(v) if isinstance(x, tuple)] if isinstance(v, tuple) else []
        for e in entries(0):   # creature objectives: {{id, text, icon}}
            r = row_list(e); pts = {}
            if r and isinstance(r[0], int): merge(pts, npc_sp(r[0]))
            objl.append((0, r[0] if r and isinstance(r[0], int) else 0, pts))
        for e in entries(1):
            r = row_list(e); pts = {}
            if r and isinstance(r[0], int): merge(pts, obj_sp(r[0]))
            objl.append((1, r[0] if r and isinstance(r[0], int) else 0, pts))
        for e in entries(2):
            r = row_list(e); pts = {}
            if r and isinstance(r[0], int): merge(pts, item_sp(r[0]))
            objl.append((2, r[0] if r and isinstance(r[0], int) else 0, pts))
        for e in entries(4):   # kill credit: {{ids...}, baseCreatureId, text, icon}
            r = row_list(e); pts = {}
            ids = row_list(r[0]) if r and isinstance(r[0], tuple) else []
            for i in ids:
                if isinstance(i, int): merge(pts, npc_sp(i))
            base = r[1] if len(r) > 1 and isinstance(r[1], int) else (ids[0] if ids and isinstance(ids[0], int) else 0)
            objl.append((0, base, pts))
        if isinstance(trig, tuple):   # {text, {[zone] = {{x, y}, ...}}}
            tr = row_list(trig)
            if len(tr) > 1 and isinstance(tr[1], tuple):
                pts = {}
                for z, ps in tr[1][0].items():
                    pts[z] = [(p[1][0], p[1][1]) for p in ps[1] if isinstance(p, tuple) and len(p[1]) >= 2 and 0 <= p[1][0] <= 100 and 0 <= p[1][1] <= 100]
                objl.append((3, 0, pts))
        sp, ep = flat(start_pts), flat(end_pts)
        if not sp and not ep and not any(o[2] for o in objl): continue
        out = bytearray(struct.pack("<BBBB", len(sp), len(ep), min(len(objl), 8), 0))
        for z, x, y in sp + ep:
            out += struct.pack("<HHH", z, int(x * 100), int(y * 100))
        for kind, eid, pts in objl[:8]:
            bl = dots(pts)
            nobj += 1; nblob += len(bl)
            out += struct.pack("<BBHI", kind, len(bl), 0, eid)
            for z, cx, cy, r, _n in bl:
                out += struct.pack("<HHHH", z, int(cx * 100), int(cy * 100), int(r * 100))
        recs[qid] = bytes(out)
    ids = sorted(recs)
    body = bytearray(); offs = {}
    base = 16 + 8 * len(ids)
    for i in ids: offs[i] = base + len(body); body += recs[i]
    with open(dst, "wb") as f:
        f.write(struct.pack("<4sIII", b"WQDB", 1, len(ids), 16))
        for i in ids: f.write(struct.pack("<II", i, offs[i]))
        f.write(body)
    print("questdb: %d quests, %d objectives, %d blobs, %.2f MB -> %s" % (len(ids), nobj, nblob, (base + len(body)) / 1048576.0, dst))

if __name__ == "__main__": main()
