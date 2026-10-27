"""Checks a pack + transports.txt built from gen_transport_data.py: the ship display, its deck, the path nodes."""
import json, struct, sys
pack, txt, exp = sys.argv[1], sys.argv[2], json.load(open(sys.argv[3]))
b = open(pack, "rb").read()
magic, ver, n, idx = struct.unpack_from("<4I", b, 0)
E = {}
for i in range(n):
    t, eid, off, size = struct.unpack_from("<4I", b, idx + i * 16)
    E[(t, eid)] = (off, size)
F = lambda s: struct.unpack("<I", s.encode())[0]
bad = 0
def check(cond, what):
    global bad
    print("ok  " if cond else "FAIL", what); bad += not cond
d = E.get((F("CDSP"), 0x80000000 | exp["display"]))
check(d is not None and d[1] == 96, "ship display 0x80000000|3015 packed")
if d:
    mid, scale = struct.unpack_from("<If", b, d[0])
    check((F("CMDL"), mid) in E, "ship model packed")
    check(abs(scale - 1.0) < 1e-6, "scale 1")
check((F("CDSP"), 0x80000000 | 3016) not in E, "the chest display is not a transport")
lines = open(txt).read().splitlines()
T = [l.split("\t") for l in lines if l.startswith("T\t")]
M = [l.split("\t") for l in lines if l.startswith("M\t")]
check(len(T) == exp["pathNodes"], f"{len(T)} path nodes")
check(any(t[1] == "9" and t[2] == "1" and t[7] == "2" and t[8] == "10" for t in T), "ferry stop node with flags 2 / delay 10")
check(len(M) == 1 and int(M[0][1]) == exp["display"], f"one deck line {M}")
if M:
    v = [float(x) for x in M[0][2:]]
    check(abs(v[0] - exp["deckZ"]) < 0.01, f"deck z {v[0]} (the larger deck level, not the cabin roof)")
    check(abs(v[1] - exp["x0"]) < 0.01 and abs(v[2] - exp["y0"]) < 0.01 and abs(v[3] - exp["x1"]) < 0.01 and abs(v[4] - exp["y1"]) < 0.01,
          f"deck rectangle {v[1:]}")
print("TRANSPORT CONVERT:", "FAIL" if bad else "PASS")
sys.exit(1 if bad else 0)
