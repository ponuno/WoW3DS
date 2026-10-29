"""Independent reference for GRND (6.18a): the height / hole / normal of the ground at random points, found by brute force
over explicit triangles (four around the centre of every square, as WoWee terrain_mesh.cpp builds them) - no closed-form
selection like w3d_ground.h.  Writes 'x y z hole nz' lines; also reports how wrong the old corner-only field would be.

    python3 ground_ref.py <points.txt>
"""
import math, random, sys
sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import terrain_rough as R

TS = 533.33333
Q = TS / 128.0
MINX, MINY = (32.0 - 48 - 1.0) * TS, (32.0 - 32 - 1.0) * TS      # tile (32, 48): tileX = 32, tileY = 48


def to_render(gr, gc):
    return MINX + (1.0 - gr / 128.0) * TS, MINY + (1.0 - gc / 128.0) * TS


def square_tris(cy, cx, r, c):
    """four triangles (render-space vertices) of square (r, c) of chunk (cx, cy), from the chunk's own 145 heights"""
    h = R.mcvt(cy, cx)
    gr0, gc0 = cy * 8 + r, cx * 8 + c
    def v(dr, dc, idx):
        x, y = to_render(gr0 + dr, gc0 + dc)
        return (x, y, R.BASE + h[idx])
    A, B = v(0, 0, r * 17 + c), v(0, 1, r * 17 + c + 1)
    C, D = v(1, 0, (r + 1) * 17 + c), v(1, 1, (r + 1) * 17 + c + 1)
    M = v(0.5, 0.5, r * 17 + 9 + c)
    return [(M, A, B), (M, B, D), (M, D, C), (M, C, A)]


def inside(p, t):
    (x0, y0, _), (x1, y1, _), (x2, y2, _) = t
    d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
    a = ((y1 - y2) * (p[0] - x2) + (x2 - x1) * (p[1] - y2)) / d
    b = ((y2 - y0) * (p[0] - x2) + (x0 - x2) * (p[1] - y2)) / d
    return a >= -1e-9 and b >= -1e-9 and 1 - a - b >= -1e-9, a, b


def sample(x, y):
    gr, gc = (1.0 - (x - MINX) / TS) * 128.0, (1.0 - (y - MINY) / TS) * 128.0
    cy, cx = min(int(gr // 8), 15), min(int(gc // 8), 15)
    r, c = min(int(gr - cy * 8), 7), min(int(gc - cx * 8), 7)
    for t in square_tris(cy, cx, r, c):
        ok, a, b = inside((x, y), t)
        if ok:
            z = a * t[0][2] + b * t[1][2] + (1 - a - b) * t[2][2]   # a, b, 1-a-b: weights of vertices 0, 1, 2
            e1 = [t[1][i] - t[0][i] for i in range(3)]
            e2 = [t[2][i] - t[0][i] for i in range(3)]
            n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
            nz = abs(n[2]) / math.sqrt(sum(k * k for k in n))
            hole = (R.HOLES.get((cy, cx), 0) >> ((r // 2) * 4 + (c // 2))) & 1
            return z, hole, nz
    raise SystemExit("no triangle at %f %f" % (x, y))


def old_field(x, y):
    """what the 129x129 bilinear field of the drawn corners gave: corners only, bilinear in the square"""
    gr, gc = (1.0 - (x - MINX) / TS) * 128.0, (1.0 - (y - MINY) / TS) * 128.0
    gr0, gc0 = int(gr), int(gc)
    fr, fc = gr - gr0, gc - gc0
    def corner(r, c):
        cy, cx = min(r // 8, 15), min(c // 8, 15)
        return R.BASE + R.height(r, c)
    return ((corner(gr0, gc0) * (1 - fc) + corner(gr0, gc0 + 1) * fc) * (1 - fr) + (corner(gr0 + 1, gc0) * (1 - fc) + corner(gr0 + 1, gc0 + 1) * fc) * fr)


random.seed(618)
worst_old = 0.0
count = 0
with open(sys.argv[1], "w") as f:
    for _ in range(6100):
        x = MINX + random.uniform(0.5, TS - 0.5)
        y = MINY + random.uniform(0.5, TS - 0.5)
        gr, gc = (1.0 - (x - MINX) / TS) * 128.0, (1.0 - (y - MINY) / TS) * 128.0
        if min(gr % 1.0, 1 - gr % 1.0, gc % 1.0, 1 - gc % 1.0) < 0.01:
            continue      # on the border of two squares: the heights agree, the normals do not
        if abs(abs(gr % 1.0 - 0.5) - abs(gc % 1.0 - 0.5)) < 0.01:
            continue      # on a diagonal of a square: two triangles meet there, either normal is right
        z, hole, nz = sample(x, y)
        worst_old = max(worst_old, abs(old_field(x, y) - z))
        count += 1
        f.write("%.5f %.5f %.5f %d %.6f\n" % (x, y, z, hole, nz))
print("reference: %d points, the old corner-only field is off by up to %.2f yd" % (count, worst_old))
assert worst_old > 0.2, "negative control: the rough ground must expose the old field"
