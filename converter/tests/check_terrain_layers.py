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
    key = tuple(want[c] for c in t)
    assert key in found, f"no batch with textures {t}: {sorted(found)}"
    assert found[key] == {(g, a)}, f"{t}: (g, a) = {found[key]}, want {(g, a)}"
expect("ACD", 128, 255)   # four layers: B (alpha 20) is the weakest and is dropped; C then D keep their order
expect("ABB", 0, 255)     # two layers
expect("AAA", 0, 0)       # one layer
print("TERRAIN LAYERS: PASS", len(found), "batches")
