"""Checks every MODL in a pack: indices in range, and triangles wound CCW seen
from their vertex normals (what GPU_CULL_BACK_CCW keeps). Prints per-model
agreement; fails if any model is below 90 %."""
import struct, sys
b = open(sys.argv[1], "rb").read()
magic, ver, n, idx = struct.unpack_from("<4I", b, 0)
bad = 0; checked = 0
for i in range(n):
    t, eid, off, size = struct.unpack_from("<4I", b, idx + i * 16)
    if t != struct.unpack("<I", b"MODL")[0] and t != struct.unpack(">I", b"MODL")[0]:
        continue
    h = struct.unpack_from("<3f3f3f3ff4I4I", b, off)
    ps, po = h[0:3], h[3:6]
    vc, ic, sc, tc, oT, oS, oV, oI = h[13:21]
    V = [struct.unpack_from("<4h4b2h", b, off + oV + k * 16) for k in range(vc)]
    I = struct.unpack_from(f"<{ic}H", b, off + oI)
    if any(x >= vc for x in I): print(f"FAIL {eid:08x}: index out of range"); bad += 1; continue
    pos = [(v[0] * ps[0] + po[0], v[1] * ps[1] + po[1], v[2] * ps[2] + po[2]) for v in V]
    agree = tot = 0.0
    for s in range(sc):
        tex, blend, flags, first, count = struct.unpack_from("<HBBII", b, off + oS + s * 12)
        for k in range(first, first + count - 2, 3):
            a, c, d = I[k], I[k + 1], I[k + 2]
            e1 = [pos[c][j] - pos[a][j] for j in range(3)]; e2 = [pos[d][j] - pos[a][j] for j in range(3)]
            g = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            nsum = [V[a][4 + j] + V[c][4 + j] + V[d][4 + j] for j in range(3)]
            dot = sum(g[j] * nsum[j] for j in range(3)); area = sum(x * x for x in g) ** 0.5
            tot += area; agree += area if dot > 0 else 0
    frac = agree / tot if tot else 1
    checked += 1
    ok = frac >= 0.9
    bad += not ok
    print(f"{'ok  ' if ok else 'FAIL'} model {eid:08x}: {vc} verts, {ic // 3} tris, {frac * 100:.1f}% CCW-outward")
print(f"{checked} models checked:", "PASS" if not bad else f"{bad} FAILED")
sys.exit(1 if bad else 0)
