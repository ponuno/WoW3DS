"""Checks the pack + transports.txt of run_worldobj_tests.sh: elevator display and deck, the A lines of TransportAnimation.dbc,
the default object set (6.12: every object, the boulder too) and the door animations kept in its model."""
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
dsp = lambda did: E.get((F("CDSP"), 0x80000000 | did))
check(dsp(exp["elevator"]) is not None, "elevator display packed (keyword elevator)")
check(dsp(exp["door"]) is not None, "door display packed (default set: door)")
check(dsp(exp["mailbox"]) is not None, "mailbox display packed (default set: mailbox)")
check(dsp(exp["boulder"]) is not None, "boulder display packed (6.12: every object model by default)")
lines = open(txt).read().splitlines()
A = [l.split("\t") for l in lines if l.startswith("A\t")]
M = [l.split("\t") for l in lines if l.startswith("M\t")]
check(len(A) == exp["animRows"], f"{len(A)} A lines")
check(sorted({a[1] for a in A}) == ["20650", "20651"], "two elevator entries")
check(any(a[1] == "20650" and a[2] == "10000" and abs(float(a[3]) - 2.0) < 1e-3 and abs(float(a[5]) + 40.0) < 1e-3 for a in A), "A 20650 10000 2 0 -40")
S = [l.split("\t") for l in lines if l.startswith("S\t")]
check(sorted(int(m[1]) for m in M) == sorted([exp["elevator"], exp["tram"]]), f"deck lines: the elevator and the tram car {M}")
byid = {int(m[1]): m for m in M}
if exp["elevator"] in byid:
    check(abs(float(byid[exp["elevator"]][2]) - exp["deckZ"]) < 0.01, f"elevator deck z {byid[exp['elevator']][2]}")
if exp["tram"] in byid:   # 6.14: no collision mesh - the drawn floor is the deck
    m = byid[exp["tram"]]
    check(abs(float(m[2]) - exp["tramZ"]) < 0.01 and abs(float(m[3]) + 3) < 0.01 and abs(float(m[5]) - 3) < 0.01, f"tram deck from the drawn floor {m[2:]}")
check([int(x[1]) for x in S] == [exp["bridge"]], f"one platform among the objects: the bridge (not the post, not the collision-less rug) {S}")
if S:
    check(abs(float(S[0][2]) - exp["bridgeZ"]) < 0.01 and abs(float(S[0][3]) + 4) < 0.01 and abs(float(S[0][5]) - 4) < 0.01, f"bridge platform {S[0][2:]}")
d = dsp(exp["door"])
if d:
    mid = struct.unpack_from("<I", b, d[0])[0]
    off, size = E[(F("CMDL"), mid)]
    h = struct.unpack_from("<14f8I8I", b, off)
    seqCount, offSeqs = h[14 + 5], h[22 + 6]
    ids = sorted(struct.unpack_from("<H", b, off + offSeqs + 16 * k)[0] for k in range(seqCount))
    check(ids == sorted(exp["doorSeqs"]), f"door sequences {ids}")
print("WORLD OBJECTS CONVERT:", "FAIL" if bad else "PASS")
sys.exit(1 if bad else 0)
