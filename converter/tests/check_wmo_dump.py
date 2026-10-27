"""Compares w3dconv --dump-placements output with gen_wmo_data.py expectations."""
import json, math, sys
exp = json.load(open(sys.argv[2]))
lines = [l.split() for l in open(sys.argv[1])]
def rot(axis, a):
    c, s = math.cos(a), math.sin(a)
    if axis == "x": return [[1, 0, 0], [0, c, -s], [0, s, c]]
    if axis == "y": return [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]
def mm(a, b): return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
def R_of(e): return mm(mm(rot("z", e[2]), rot("y", e[1])), rot("x", e[0]))
bad = 0
groups = {"house": [l for l in lines if l[0] == "group" and "House" in l[1]],
          "tower": [l for l in lines if l[0] == "group" and "Tower" in l[1]]}
for k in ("house", "tower"):
    if len(groups[k]) != len(exp[k]["groups"]): print("FAIL", k, "group count", len(groups[k])); bad += 1; continue
    for got, want in zip(groups[k], exp[k]["groups"]):
        pos = list(map(float, got[4:7])); r = list(map(float, got[8:11]))
        d = math.dist(pos, want["pos"]); dr = max(abs(a - b) for a, b in zip(R_of(r)[0] + R_of(r)[1] + R_of(r)[2],
                                                                         sum(R_of(want["rot"]), [])))
        ok = d < 0.01 and dr < 1e-4
        bad += not ok
        print("ok  " if ok else "FAIL", k, "group pos err %.5f yd, rotation err %.2e" % (d, dr))
dd = [l for l in lines if l[0] == "doodad"]
if len(dd) != len(exp["house"]["doodads"]): print("FAIL doodad count", len(dd)); bad += 1
for got, want in zip(dd, exp["house"]["doodads"]):
    pos = list(map(float, got[4:7])); r = list(map(float, got[8:11])); sc = float(got[12])
    R = R_of(r)
    dr = max(abs(R[i][j] - want["R"][i][j]) for i in range(3) for j in range(3))
    d = math.dist(pos, want["pos"])
    ok = d < 0.01 and dr < 1e-4 and abs(sc - want["scale"]) < 1e-4
    bad += not ok
    print("ok  " if ok else "FAIL", "doodad", got[1], "pos err %.5f, rotation err %.2e, scale %.3f/%.3f" % (d, dr, sc, want["scale"]))
print("WMO placements:", "PASS" if bad == 0 else f"{bad} FAILED")
sys.exit(1 if bad else 0)
