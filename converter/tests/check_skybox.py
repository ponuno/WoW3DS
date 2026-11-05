"""6.61: a pack from w3dconv --skybox: one SKYB entry (LightSkybox 5) naming a MODL that exists with the dome's 4 triangles... 12 indices;
the model whose file is missing (6) has none. check_skybox.py <pack.w3dp>"""
import struct, sys
b = open(sys.argv[1], "rb").read()
magic, ver, n, idx = struct.unpack_from("<4I", b, 0)
ents = {}
for i in range(n):
    t, eid, off, size = struct.unpack_from("<4I", b, idx + i * 16)
    ents.setdefault(struct.pack("<I", t)[::-1] if False else bytes(struct.pack("<I", t)), {})[eid] = (off, size)
def kind(name):   # the fourcc as stored (little endian of the characters, or big): try both
    for k in (name.encode(), name.encode()[::-1]):
        if k in ents: return ents[k]
    return {}
sk, md = kind("SKYB"), kind("MODL")
ok = True
def check(c, m):
    global ok
    print(("ok   " if c else "FAIL ") + m)
    ok = ok and c
check(set(sk) == {5}, "one sky model entry, id 5 (got %s)" % sorted(sk))
if 5 in sk:
    off, size = sk[5]
    model, _ = struct.unpack_from("<2I", b, off)
    check(model in md, "its MODL %08x is in the pack" % model)
    if model in md:
        moff, msize = md[model]
        h = struct.unpack_from("<3f3f3f3ff4I4I", b, moff)
        vc, ic, sc, tc = h[13:17]
        check(ic == 12 and sc == 1 and tc == 1, "a dome of 4 triangles, 1 submesh, 1 texture (%d indices, %d submeshes, %d textures)" % (ic, sc, tc))
        check(h[12] > 100.0, "its radius is the model's (%.1f)" % h[12])
print("SKYBOX PACK:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
