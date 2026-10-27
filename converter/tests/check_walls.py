"""Independent check of the random walks coll_test replayed through w3d_collide_move().

    python3 check_walls.py <expect.json> <trajectories.txt>

expect.json "walls" = every steep triangle of the synthetic world (render space, computed by
gen_wmo_data.py from the source geometry, not from the pack). For each walk (feet z), each wall is
cut to the slab z + 0.75 .. z + 2.0 and projected to XY; then
  1. every position after the first move keeps at least r - tol from every wall piece;
  2. no step's straight chord passes through a wall piece deeper than r from the piece's ends
     (a walker sliding round the end of a fence may cut its corner, it never goes through the middle).
"""
import json, math, sys

exp = json.load(open(sys.argv[1]))
R = exp["walker_radius"]
TOL = 0.02          # 8 mm XY quantisation in the pack + float rounding

def clip_slab(tri, lo, hi):
    poly = [tuple(v) for v in tri]
    for plane in (0, 1):
        out = []
        for i, cur in enumerate(poly):
            prev = poly[i - 1]
            dc = cur[2] - lo if plane == 0 else hi - cur[2]
            dp = prev[2] - lo if plane == 0 else hi - prev[2]
            if (dc >= 0) != (dp >= 0):
                t = dp / (dp - dc)
                out.append(tuple(prev[k] + (cur[k] - prev[k]) * t for k in range(3)))
            if dc >= 0: out.append(cur)
        poly = out
        if not poly: break
    return [(p[0], p[1]) for p in poly]

def seg_dist(p, a, b):
    ex, ey = b[0] - a[0], b[1] - a[1]
    l2 = ex * ex + ey * ey
    t = 0.0 if l2 < 1e-12 else max(0.0, min(1.0, ((p[0] - a[0]) * ex + (p[1] - a[1]) * ey) / l2))
    return math.hypot(p[0] - a[0] - ex * t, p[1] - a[1] - ey * t)

def area2(poly):
    return sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1] for i in range(len(poly)))

def inside(poly, p):
    A = area2(poly)
    if len(poly) < 3 or abs(A) < 1e-6: return False
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        c = (b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0])
        if (A > 0 and c < 0) or (A < 0 and c > 0): return False
    return True

def dist(poly, p):
    if inside(poly, p): return 0.0
    return min(seg_dist(p, poly[i], poly[(i + 1) % len(poly)]) for i in range(len(poly)))

def seg_x(a, b, c, d):
    """Intersection parameter point of segments ab and cd, or None."""
    r = (b[0] - a[0], b[1] - a[1]); s = (d[0] - c[0], d[1] - c[1])
    den = r[0] * s[1] - r[1] * s[0]
    if abs(den) < 1e-12: return None
    t = ((c[0] - a[0]) * s[1] - (c[1] - a[1]) * s[0]) / den
    u = ((c[0] - a[0]) * r[1] - (c[1] - a[1]) * r[0]) / den
    if 0 <= t <= 1 and 0 <= u <= 1: return (a[0] + r[0] * t, a[1] + r[1] * t)
    return None

def deep_crossing(poly, a, b):
    """A point where chord ab meets the piece, more than R inside its ends along its long axis."""
    pts = []
    if inside(poly, a): pts.append(a)
    if inside(poly, b): pts.append(b)
    for i in range(len(poly)):
        x = seg_x(a, b, poly[i], poly[(i + 1) % len(poly)])
        if x: pts.append(x)
    if not pts: return None
    # long axis = the two piece vertices farthest apart
    best, e0, e1 = -1, poly[0], poly[0]
    for i in range(len(poly)):
        for j in range(i + 1, len(poly)):
            d = math.dist(poly[i], poly[j])
            if d > best: best, e0, e1 = d, poly[i], poly[j]
    if best < 2 * R: return None
    ux, uy = (e1[0] - e0[0]) / best, (e1[1] - e0[1]) / best
    for p in pts:
        s = (p[0] - e0[0]) * ux + (p[1] - e0[1]) * uy
        if R < s < best - R: return p
    return None

walks = []
for line in open(sys.argv[2]):
    f = line.split()
    if f[0] == "walk": walks.append((float(f[2]), [])); continue
    walks[-1][1].append((float(f[0]), float(f[1])))

pieces_by_z = {}
bad = checked = chords = 0
worst = 1e9
for wi, (z, pts) in enumerate(walks):
    if z not in pieces_by_z:
        ps = []
        for t in exp["walls"]:
            poly = clip_slab(t, z + 0.75, z + 2.0)
            if len(poly) >= 2:
                xs = [p[0] for p in poly]; ys = [p[1] for p in poly]
                ps.append((poly, min(xs), max(xs), min(ys), max(ys)))
        pieces_by_z[z] = ps
    pieces = pieces_by_z[z]
    for k in range(1, len(pts)):
        p, q = pts[k], pts[k - 1]
        bx0, bx1 = min(p[0], q[0]) - 1, max(p[0], q[0]) + 1
        by0, by1 = min(p[1], q[1]) - 1, max(p[1], q[1]) + 1
        for poly, x0, x1, y0, y1 in pieces:
            if x1 < bx0 or x0 > bx1 or y1 < by0 or y0 > by1: continue
            d = dist(poly, p)
            worst = min(worst, d)
            checked += 1
            if d < R - TOL:
                bad += 1
                if bad <= 10: print(f"FAIL walk {wi} step {k}: {d:.4f} yd from a wall at ({p[0]:.3f}, {p[1]:.3f})")
            if k >= 2:
                chords += 1
                x = deep_crossing(poly, q, p)
                if x:
                    bad += 1
                    if bad <= 10: print(f"FAIL walk {wi} step {k}: went through a wall at ({x[0]:.3f}, {x[1]:.3f})")
moves = sum(len(p) - 1 for _, p in walks)
print(f"random walks: {len(walks)} walks, {moves} moves, {checked} wall distances (closest {worst:.4f} yd, "
      f"radius {R}), {chords} chords")
print("WALL WALKS:", "PASS" if bad == 0 and len(walks) > 0 else f"{bad} FAILED")
sys.exit(1 if bad or not walks else 0)
