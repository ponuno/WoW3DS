"""Reads the terrain entry of a pack made from gen_terrain_layers.py and checks the ground splat (3 textures, 2 weights)."""
import struct, sys

def fnv(path):
    h = 0x811C9DC5
    for c in path.lower().replace("\\", "/").encode():
        h = ((h ^ c) * 0x01000193) & 0xFFFFFFFF
    return h

b = open(sys.argv[1], "rb").read()
magic, ver, n, ioff = struct.unpack_from("<4I", b, 0)
terr = None
for i in range(n):
    t, id_, off, size = struct.unpack_from("<4I", b, ioff + 16 * i)
    if t == struct.unpack("<I", b"TERR")[0]:
        terr = b[off:off + size]
assert terr, "no terrain entry"
(tx, ty) = struct.unpack_from("<2I", terr, 0)
f = struct.unpack_from("<9f", terr, 8)
grid, vc, ic, bc, tc, offTex, offB, offV, offI, offT2 = struct.unpack_from("<10I", terr, 44)
assert offTex >= 84 and offT2, "no third texture table"
ids = struct.unpack_from(f"<{tc}I", terr, offTex)
tex2 = struct.unpack_from(f"<{bc}H", terr, offT2)
want = {n: ids.index(fnv(f"textures/test/{n}.blp")) for n in "ABCD"}
verts = [struct.unpack_from("<4h4B2h", terr, offV + 16 * k) for k in range(vc)]
idx = struct.unpack_from(f"<{ic}H", terr, offI)
found = {}
for k in range(bc):
    o = offB + 52 * k
    t0, t1 = struct.unpack_from("<2H", terr, o)
    first, count = struct.unpack_from("<I", terr, o + 4)[0], struct.unpack_from("<I", terr, o + 16)[0]
    used = set(idx[first:first + count])
    gs = {(verts[v][5], verts[v][7]) for v in used}   # (g, a)
    found[(t0, t1, tex2[k])] = gs
print("textures", want)
def expect(t, g, a):
    # (5.50: the vertices on a chunk border are harmonised with the neighbour's, which can also put the neighbour's textures
    # into a chunk's free slots - so look for a batch that has these textures and the chunk's own interior weights)
    need = {want[c] for c in t}
    ok = [k for k, gs in found.items() if need <= set(k) and (g, a) in gs]
    assert ok, f"{t}: no batch with these textures and (g, a) = {(g, a)}: {sorted(found.items())}"
expect("ACD", 128, 255)   # four layers: B (alpha 20) is the weakest and is dropped; C then D keep their order
expect("AB", 0, 255)      # two layers
expect("A", 0, 0)         # one layer

# 5.50: the vertices that share a position (the borders of chunks) look the same from every side
import collections
byPos = collections.defaultdict(list)
for k in range(bc):
    o = offB + 52 * k
    t0, t1 = struct.unpack_from("<2H", terr, o)
    first, count = struct.unpack_from("<I", terr, o + 4)[0], struct.unpack_from("<I", terr, o + 16)[0]
    for v in set(idx[first:first + count]):
        x, y, z, _, c0, cg, c2, ca, u, w = verts[v]
        g_, a_ = cg / 255.0, ca / 255.0
        wt = collections.defaultdict(float)
        for tex, wgt in ((t0, (1 - a_) * (1 - g_)), (t1, a_ * (1 - g_)), (tex2[k], g_)):
            wt[tex] += wgt
        byPos[(x, y)].append(((t0, t1, tex2[k]), wt))
shared = worst = 0
for pos, ent in byPos.items():
    if len({e[0] for e in ent}) < 2:
        continue
    shared += 1
    for tex in set().union(*[e[1].keys() for e in ent]):
        vals = [e[1].get(tex, 0.0) for e in ent]
        worst = max(worst, max(vals) - min(vals))
assert shared > 0, "no shared chunk-border vertices between different batches"
assert worst < 0.06, f"ground tears along a chunk border: texture weights differ by {worst:.2f} at a shared vertex"
print("seams: %d shared border vertices, worst weight difference %.3f" % (shared, worst))
# 6.13: every LOD covers the same ground (a coarse block touching a hole is split, not dropped)
area = [0.0, 0.0, 0.0]
for k in range(bc):
    o = offB + 52 * k
    firsts = struct.unpack_from("<3I", terr, o + 4)
    counts = struct.unpack_from("<3I", terr, o + 16)
    for l in range(3):
        for t in range(firsts[l], firsts[l] + counts[l] - 2, 3):
            a_, b_, c_ = verts[idx[t]], verts[idx[t + 1]], verts[idx[t + 2]]
            area[l] += abs((b_[0] - a_[0]) * (c_[1] - a_[1]) - (b_[1] - a_[1]) * (c_[0] - a_[0]))
assert area[0] > 0 and abs(area[1] - area[0]) < area[0] * 1e-6 and abs(area[2] - area[0]) < area[0] * 1e-6, f"ground area by LOD {area}"
print("holes: ground area equal on all LODs (%.0f)" % area[0])
print("TERRAIN LAYERS: PASS", len(found), "batches")
