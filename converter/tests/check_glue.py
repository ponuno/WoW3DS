"""Stage 5.29: GLUE entries (login / character scenes) and glue.txt against gen_creature_data.py.
    check_glue.py <pack.w3dp> <expect.json> <glue.txt>"""
import json, struct, sys
b = open(sys.argv[1], "rb").read(); exp = json.load(open(sys.argv[2]))["glue"]; txt = open(sys.argv[3], encoding="utf-8").read()
magic, ver, n, idx = struct.unpack_from("<4I", b, 0)
E = {}
for i in range(n):
    t, eid, off, size = struct.unpack_from("<4I", b, idx + i * 16)
    E[(t, eid)] = (off, size)
fourcc = lambda s: struct.unpack("<I", s.encode())[0]
bad = 0
def check(ok, what):
    global bad
    print("ok  " if ok else "FAIL", what); bad += not ok
glue = {eid: E[k] for k in E if k[0] == fourcc("GLUE") for eid in [k[1]]}
check(sorted(glue) == exp["scenes"], f"scenes {sorted(glue)}")
for sid, (off, size) in glue.items():
    model, = struct.unpack_from("<I", b, off)
    cam = struct.unpack_from("<6f", b, off + 4); fov, near, far = struct.unpack_from("<3f", b, off + 28)
    stand = struct.unpack_from("<3f", b, off + 40); flags, radius = struct.unpack_from("<If", b, off + 52)
    check((fourcc("MODL"), model) in E, f"scene {sid}: model {model:08x} packed")
    check(flags & 2 and abs(fov - 0.95) < 1e-4, f"scene {sid}: camera {cam} fov {fov:.2f}")
    if sid == 1:
        check(flags & 1 and all(abs(a - e) < 1e-3 for a, e in zip(stand, exp["stand"])), f"scene 1: stand {stand}")
    else:
        check(flags & 1 and all(abs(a - e) < 1e-3 for a, e in zip(stand, cam[3:6])), f"scene 0: no mark, stand = target {stand}")
    check(radius > 200 if sid == 1 else radius > 1, f"scene {sid}: radius {radius:.1f}")
lines = txt.splitlines()
check("T\tRACE_INFO_HUMAN\tHumans are a test race.\\nSecond line with a \"quote\"." in lines, "GlueStrings: escapes")
check("T\tCLASS_INFO_WARRIOR\tWarriors hit things." in lines and not any("NOT_A_STRING" in l for l in lines), "GlueStrings: only strings")
want = "O\t1\t1\t0\t" + "\t".join(f"{d}\t{i}" for d, i in exp["outfit"])
check(want in lines, "start outfit " + want.replace("\t", " "))
check("Z\t12\tElwynn Forest" in lines and "Z\t9\tNorthshire Valley" in lines, "zone names")
print("GLUE:", "PASS" if not bad else f"{bad} FAILED"); sys.exit(1 if bad else 0)
