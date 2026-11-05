#!/usr/bin/env python3
"""questdb.bin (version 2, world coordinates) from the TrinityCore Forever database (the world SQL of advocaite/TrinityCore, release
forever-tdb-*). Same record layout as questie_import.py (version 1), but a point is {u16 map id, i16 x, i16 y} and a blob
{u16 map id, i16 x, i16 y, u16 radius}, x / y being the server's position_x / position_y in yards (the client needs no zone maps).

    tdb_import.py <TDB_full_world_forever_*.sql> <out questdb.bin>

Per quest of quest_template_classic_level: where it is given / handed in (creature_queststarter / _questender and the gameobject ones,
at the spawns of those), and per objective of quest_objectives: monsters (their spawns, with KillCredit1/2 creatures), game objects (their
spawns), items (the spawns of the creatures whose loot has the item, references followed, and of the game objects whose chest loot has it).
Spawns are thinned on a grid to at most 60 dots an objective. TrinityCore data is GPL-2.
"""
import struct, sys, collections
from sqlrows import rows, columns

KILL, OBJECT, ITEM, EXPLORE = 0, 1, 2, 3
MAXDOTS = 60

def col(path, table):
    c = columns(path, table)
    return {n: i for i, n in enumerate(c)}

def thin(points, maxdots):
    """points: [(map, x, y)] -> [(map, x, y, r)] on a grid that gets coarser until there are at most maxdots cells"""
    if not points: return []
    cell = 20
    while True:
        g = collections.defaultdict(list)
        for m, x, y in points: g[(m, int(x // cell), int(y // cell))].append((x, y))
        if len(g) <= maxdots or cell >= 5120: break
        cell *= 2
    out = []
    for (m, _, _), v in g.items():
        cx = sum(p[0] for p in v) / len(v); cy = sum(p[1] for p in v) / len(v)
        r = max(6, int(cell * 0.6)) if len(v) > 1 or cell > 20 else 6
        out.append((m, int(round(cx)), int(round(cy)), min(r, 3000)))
    out.sort()
    return out

def main(src, dst):
    classic = set(r[0] for r in rows(src, 'quest_template_classic_level'))
    print('classic quests', len(classic))
    cc = col(src, 'creature'); cs = collections.defaultdict(list)
    for r in rows(src, 'creature'): cs[r[cc['id']]].append((r[cc['map']], r[cc['position_x']], r[cc['position_y']]))
    gc = col(src, 'gameobject'); gs = collections.defaultdict(list)
    for r in rows(src, 'gameobject'): gs[r[gc['id']]].append((r[gc['map']], r[gc['position_x']], r[gc['position_y']]))
    print('spawned creature ids', len(cs), 'object ids', len(gs))
    starters = {'c': collections.defaultdict(set), 'g': collections.defaultdict(set)}
    enders = {'c': collections.defaultdict(set), 'g': collections.defaultdict(set)}
    for tab, d, k in (('creature_queststarter', starters, 'c'), ('gameobject_queststarter', starters, 'g'),
                      ('creature_questender', enders, 'c'), ('gameobject_questender', enders, 'g')):
        for r in rows(src, tab):
            if r[1] in classic: d[k][r[1]].add(r[0])
    obj = collections.defaultdict(list)
    oc = col(src, 'quest_objectives')
    for r in rows(src, 'quest_objectives'):
        if r[oc['QuestID']] in classic: obj[r[oc['QuestID']]].append((r[oc['Order']], r[oc['Type']], r[oc['ObjectID']]))
    # kill credits: creature X gives credit for the objective creature
    tc = col(src, 'creature_template'); credit = collections.defaultdict(set)
    for r in rows(src, 'creature_template'):
        for k in ('KillCredit1', 'KillCredit2'):
            if r[tc[k]]: credit[r[tc[k]]].add(r[tc['entry']])
    # loot: item -> creatures / objects
    td = col(src, 'creature_template_difficulty'); lootOf = collections.defaultdict(set)   # loot id -> creature ids
    for r in rows(src, 'creature_template_difficulty'):
        if r[td['DifficultyID']] == 0 and r[td['LootID']]: lootOf[r[td['LootID']]].add(r[td['Entry']])
    gt = col(src, 'gameobject_template'); goLoot = collections.defaultdict(set)
    for r in rows(src, 'gameobject_template'):
        if r[gt['type']] == 3 and r[gt['Data1']]: goLoot[r[gt['Data1']]].add(r[gt['entry']])
    wanted = set(o[2] for q in obj.values() for o in q if o[1] == 1)
    ref_has = collections.defaultdict(set)      # reference entry -> wanted items inside
    direct = {'c': collections.defaultdict(set), 'g': collections.defaultdict(set)}     # item -> loot ids
    refs = {'c': [], 'g': []}
    lc = col(src, 'creature_loot_template')
    for tab, k in (('creature_loot_template', 'c'), ('gameobject_loot_template', 'g')):
        for r in rows(src, tab):
            ent, typ, item = r[lc['Entry']], r[lc['ItemType']], r[lc['Item']]
            if typ == 0 and item in wanted: direct[k][item].add(ent)
            elif typ == 1: refs[k].append((ent, item))
    for r in rows(src, 'reference_loot_template'):
        if r[lc['ItemType']] == 0 and r[lc['Item']] in wanted: ref_has[r[lc['Entry']]].add(r[lc['Item']])
    # references that contain a wanted item (nested references: three rounds)
    for _ in range(3):
        for k in 'cg':
            for ent, ref in refs[k]:
                for it in ref_has.get(ref, ()):
                    direct[k][it].add(ent)
        for r in rows(src, 'reference_loot_template'):
            if r[lc['ItemType']] == 1:
                for it in ref_has.get(r[lc['Item']], ()): ref_has[r[lc['Entry']]].add(it)
    qic = collections.defaultdict(set); qig = collections.defaultdict(set)      # quest item -> creatures / objects that carry it
    for r in rows(src, 'creature_questitem'): qic[r[3]].add(r[0])
    for r in rows(src, 'gameobject_questitem'): qig[r[2]].add(r[0])
    def item_spawns(item):
        pts = []
        for cid in qic.get(item, ()): pts += cs.get(cid, [])
        for gid in qig.get(item, ()): pts += gs.get(gid, [])
        for lid in direct['c'].get(item, ()):
            for cid in lootOf.get(lid, ()): pts += cs.get(cid, [])
        for lid in direct['g'].get(item, ()):
            for gid in goLoot.get(lid, ()): pts += gs.get(gid, [])
        return pts
    recs = {}
    nobj = collections.Counter()
    for q in sorted(classic):
        sp = []; ep = []
        for cid in starters['c'].get(q, ()): sp += cs.get(cid, [])
        for gid in starters['g'].get(q, ()): sp += gs.get(gid, [])
        for cid in enders['c'].get(q, ()): ep += cs.get(cid, [])
        for gid in enders['g'].get(q, ()): ep += gs.get(gid, [])
        sp = [(m, x, y) for m, x, y, _ in thin(sp, 12)]; ep = [(m, x, y) for m, x, y, _ in thin(ep, 12)]
        objs = []
        for _, typ, oid in sorted(obj.get(q, ())):
            if typ in (0, 3):
                pts = list(cs.get(oid, []))
                for c2 in credit.get(oid, ()): pts += cs.get(c2, [])
                kind = KILL
            elif typ == 2: pts = list(gs.get(oid, [])); kind = OBJECT
            elif typ == 1: pts = item_spawns(oid); kind = ITEM
            else: continue
            bl = thin(pts, MAXDOTS)
            nobj[(kind, bool(bl))] += 1
            objs.append((kind, oid, bl))
        if not (sp or ep or any(o[2] for o in objs)): continue
        b = bytearray(struct.pack('<BBBB', len(sp), len(ep), len(objs), 0))
        for m, x, y in sp + ep: b += struct.pack('<Hhh', m, max(-32767, min(32767, int(round(x)))), max(-32767, min(32767, int(round(y)))))
        for kind, oid, bl in objs:
            b += struct.pack('<BBHI', kind, len(bl), 0, oid)
            for m, x, y, r in bl: b += struct.pack('<HhhH', m, max(-32767, min(32767, x)), max(-32767, min(32767, y)), r)
        recs[q] = bytes(b)
    ids = sorted(recs)
    out = bytearray(b'WQDB' + struct.pack('<III', 2, len(ids), 16))
    off = 16 + 8 * len(ids)
    idx = bytearray(); body = bytearray()
    for q in ids:
        idx += struct.pack('<II', q, off + len(body)); body += recs[q]
    open(dst, 'wb').write(bytes(out + idx + body))
    print('quests', len(ids), 'bytes', 16 + len(idx) + len(body))
    for k, v in sorted(nobj.items()): print('  objective kind', k[0], 'with areas' if k[1] else 'WITHOUT areas', v)

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
