"""Checks CDSP/CMDL entries of a pack against gen_creature_data.py expectations."""
import json, struct, sys
b = open(sys.argv[1], "rb").read(); exp = json.load(open(sys.argv[2]))
magic, ver, n, idx = struct.unpack_from("<4I", b, 0)
E = {}
for i in range(n):
    t, eid, off, size = struct.unpack_from("<4I", b, idx + i * 16)
    E[(t, eid)] = (off, size)
fourcc = lambda s: struct.unpack("<I", s.encode())[0]
bad = 0
for did, want in exp["displays"].items():
    key = (fourcc("CDSP"), int(did))
    if key not in E: print("FAIL display", did, "missing"); bad += 1; continue
    off, size = E[key]
    model, scale = struct.unpack_from("<If", b, off); tex = struct.unpack_from("<5I", b, off + 8)
    gc = struct.unpack_from("<H", b, off + 28)[0]; geos = list(struct.unpack_from(f"<{gc}H", b, off + 30))
    errs = []
    if model != want["model"]: errs.append(f"model {model:08x} != {want['model']:08x}")
    if abs(scale - want["scale"]) > 1e-6: errs.append(f"scale {scale}")
    if "skin1" in want and tex[0] != want["skin1"]: errs.append(f"skin1 {tex[0]:08x} != {want['skin1']:08x}")
    if "body" in want and tex[3] != want["body"]: errs.append(f"body {tex[3]:08x}")
    if "hair" in want and tex[4] != want["hair"]: errs.append(f"hair {tex[4]:08x}")
    if sorted(geos) != want.get("geosets", []): errs.append(f"geosets {sorted(geos)} != {want.get('geosets', [])}")
    for t in tex:
        if t and (fourcc("TEX "), t) not in E: errs.append(f"texture {t:08x} not packed")
    if (fourcc("CMDL"), model) not in E: errs.append("model not packed")
    print("ok  " if not errs else "FAIL", "display", did, "; ".join(errs))
    bad += bool(errs)
for did in exp["missing_displays"]:
    if (fourcc("CDSP"), did) in E: print("FAIL display", did, "should be skipped"); bad += 1
# wolf model: kept sequences
off, size = E[(fourcc("CMDL"), exp["displays"]["1001"]["model"])]
h = struct.unpack_from("<14f8I8I", b, off)   # posScale, posOffset, bmin, bmax, radius, transScale; counts; offsets
seqCount, offSeqs = h[14 + 5], h[22 + 6]
ids = [struct.unpack_from("<H", b, off + offSeqs + 16 * k)[0] for k in range(seqCount)]
ok = ids == exp["wolf_seqs"]; bad += not ok
print("ok  " if ok else "FAIL", "wolf sequences", ids, "bones", h[14 + 4], "palettes", h[14 + 6])
print("CREATURE PACK:", "PASS" if not bad else f"{bad} FAILED"); sys.exit(1 if bad else 0)
