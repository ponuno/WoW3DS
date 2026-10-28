"""Synthetic game data for the WMO path of w3dconv (no client files needed).

    python3 gen_wmo_data.py <out Data dir> <expect.json>

Writes a loose-file Data/ tree (read by DataDir like an extracted client):
  World/Maps/Test/Test_32_48.adt, Test_33_48.adt   flat terrain; MODF + MDDF
  World/wmo/Test/House.wmo + House_000.wmo (exterior) + House_001.wmo (interior)
  World/wmo/Test/Tower.wmo + Tower_000.wmo          a second building
  Textures/Test/Wall.blp                           BLP2 palettised 32x32
The same House placement (uniqueId 777) is listed by both ADTs (real ADTs repeat a
WMO in every tile it overlaps); Tower (uid 778) sits in tile 34_48, outside the
converted region, and is listed only by 33_48. expect.json holds what the
converter must produce, computed here with plain Python maths.
"""
import json, math, os, struct, sys

TS = 533.33333
ZP = 32 * TS
out, expect_path = sys.argv[1], sys.argv[2]

def chunk(magic, payload):                     # file stores the magic reversed
    return magic[::-1].encode() + struct.pack("<I", len(payload)) + payload

def write(path, data):
    p = os.path.join(out, path.replace("\\", "/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data)

# ------------------------------------------------------------ texture (BLP2, palette, no alpha)
W = H = 32
pal = b"".join(struct.pack("<BBBB", 40 + i // 2, 60 + i // 3, 140 + i // 4, 255) for i in range(256))   # BGRA
pix = bytes(((x // 4 + y // 4) & 1) * 200 + (x * 3 + y) % 50 for y in range(H) for x in range(W))
hdr = b"BLP2" + struct.pack("<IBBBBII", 1, 1, 0, 0, 0, W, H)
offs = [0] * 16; sizes = [0] * 16
offs[0] = len(hdr) + 64 + 64 + 1024; sizes[0] = len(pix)
write("Textures/Test/Wall.blp", hdr + struct.pack("<16I", *offs) + struct.pack("<16I", *sizes) + pal + pix)

# ------------------------------------------------------------ WMO writer
def box_group(lo, hi, flags, mat, reverse=False, extra=(), quads=None):
    """Axis-aligned box (12 tris), one batch with material 'mat'. reverse = clockwise winding.
    quads: render these (a, b, c, d, normal) quads instead of the box (lo/hi stay the group bounds)."""
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    faces = quads if quads is not None else [((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (0, -1, 0)),
             ((x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (0, 1, 0)),
             ((x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (-1, 0, 0)),
             ((x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1), (1, 0, 0)),
             ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (0, 0, 1)),
             ((x0, y1, z0), (x1, y1, z0), (x1, y0, z0), (x0, y0, z0), (0, 0, -1))]
    V, N, T, I = [], [], [], []
    for a, b, c, d, n in faces:
        base = len(V)
        V += [a, b, c, d]; N += [n] * 4; T += [(0, 0), (1, 0), (1, 1), (0, 1)]
        I += [base, base + 2, base + 1, base, base + 3, base + 2] if reverse else [base, base + 1, base + 2, base, base + 2, base + 3]
    render_tris = len(I) // 3
    tri_flags = [0x20] * render_tris           # F_RENDER: visible and collidable
    for verts, fl in extra:                     # extra triangles outside the render batch
        base = len(V)
        V += list(verts); N += [(0, 0, 1)] * 3; T += [(0, 0)] * 3
        I += [base, base + 1, base + 2]
        tri_flags.append(fl)
    sub = b""
    sub += chunk("MOPY", b"".join(struct.pack("<BB", f, mat if f & 0x20 else 0xFF) for f in tri_flags))
    sub += chunk("MOVI", struct.pack(f"<{len(I)}H", *I))
    sub += chunk("MOVT", b"".join(struct.pack("<fff", *v) for v in V))
    sub += chunk("MONR", b"".join(struct.pack("<fff", *n) for n in N))
    sub += chunk("MOTV", b"".join(struct.pack("<ff", *t) for t in T))
    sub += chunk("MOBA", struct.pack("<6hIHHHBB", 0, 0, 0, 0, 0, 0, 0, render_tris * 3, 0, len(V) - 1, 0, mat))
    mogp = struct.pack("<III", 0, 0, flags) + struct.pack("<6f", *lo, *hi) + struct.pack("<HHHHHH", 0, 0, 0, 0, 1, 0)
    mogp += bytes(4) + struct.pack("<III", 0, 0, 0) + bytes(4)
    assert len(mogp) == 68, len(mogp)
    return chunk("MVER", struct.pack("<I", 17)) + chunk("MOGP", mogp + sub)

def wmo_root(groups, texname, doodads, sets):
    motx = texname.encode() + b"\0"
    while len(motx) % 4: motx += b"\0"
    momt = struct.pack("<16I", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)   # opaque, texture offset 0
    names = b""; name_off = {}
    for path, *_ in doodads:
        if path not in name_off: name_off[path] = len(names); names += path.encode() + b"\0"
    modd = b""
    for path, pos, quat, scale in doodads:
        modd += struct.pack("<I3f4ff", name_off[path], *pos, *quat, scale) + struct.pack("<I", 0xFFFFFFFF)
    mods = b"".join(n.encode().ljust(20, b"\0") + struct.pack("<III", s, c, 0) for n, s, c in sets)
    mogi = b"".join(struct.pack("<I6fi", fl, *lo, *hi, -1) for lo, hi, fl in groups)
    lo = [min(g[0][i] for g in groups) for i in range(3)]; hi = [max(g[1][i] for g in groups) for i in range(3)]
    mohd = struct.pack("<IIIIIIIII6fHH", 1, len(groups), 0, 0, len(name_off), len(doodads), len(sets), 0, 0, *lo, *hi, 0, 0)
    return (chunk("MVER", struct.pack("<I", 17)) + chunk("MOHD", mohd) + chunk("MOTX", motx) + chunk("MOMT", momt) +
            chunk("MOGN", b"\0\0") + chunk("MOGI", mogi) + chunk("MODN", names) + chunk("MODD", modd) + chunk("MODS", mods))

def quat_axis(ax, deg):
    s = math.sin(math.radians(deg) / 2); c = math.cos(math.radians(deg) / 2)
    return (ax[0] * s, ax[1] * s, ax[2] * s, c)            # x, y, z, w

house_groups = [((-10, -6, 0), (10, 6, 8), 0x8), ((-8, -4, 0.2), (8, 4, 6), 0x2000)]
house_doodads = [("World\\Test\\Lamp.mdx", (4.0, 2.0, 0.5), quat_axis((0, 0, 1), 40), 1.0),
                 ("World\\Test\\Barrel.mdx", (-3.0, 1.0, 0.0), quat_axis((0.6, 0, 0.8), 25), 1.5),
                 ("World\\Test\\Lamp.mdx", (0.0, -5.0, 2.0), quat_axis((1, 0, 0), 90), 0.8)]
house_sets = [("Set_$DefaultGlobal", 0, 2), ("Set_Extra", 2, 1)]
write("World/wmo/Test/House.wmo", wmo_root(house_groups, "Textures\\Test\\Wall.blp", house_doodads, house_sets))
for i, (lo, hi, fl) in enumerate(house_groups):
    write(f"World/wmo/Test/House_{i:03d}.wmo", box_group(lo, hi, fl, 0))
tower_groups = [((-3, -3, 0), (3, 3, 20), 0x8)]
# Stairs: a platform (box 8..16 x -3..3, top at z 4.5) reached by an invisible collision ramp
# (F_COLLISION only, not rendered) from z 0 at x 0 to z 4.5 at x 8, plus a carpet on the
# platform flagged F_RENDER|F_DETAIL (rendered in the real game, never collidable) 1 yd up.
ramp = [(((0, -2, 0), (8, -2, 4.5), (8, 2, 4.5)), 0x08), (((0, -2, 0), (8, 2, 4.5), (0, 2, 0)), 0x08)]
carpet = [(((9, -1, 5.5), (15, -1, 5.5), (15, 1, 5.5)), 0x24), (((9, -1, 5.5), (15, 1, 5.5), (9, 1, 5.5)), 0x24)]
stairs_groups = [((8, -3, 0), (16, 3, 4.5), 0x8)]
write("World/wmo/Test/Stairs.wmo", wmo_root(stairs_groups, "Textures\\Test\\Wall.blp", [], [("Set_$DefaultGlobal", 0, 0)]))
write("World/wmo/Test/Stairs_000.wmo", box_group(*stairs_groups[0][:2], 0x8, 0, extra=ramp + carpet))
write("World/wmo/Test/Tower.wmo", wmo_root(tower_groups, "Textures\\Test\\Wall.blp", [], [("Set_$DefaultGlobal", 0, 0)]))
write("World/wmo/Test/Tower_000.wmo", box_group(*tower_groups[0][:2], 0x8, 0, reverse=True))   # converter must flip it

# Hut (walls, stage 5 collision): 10 x 8 room, 4 yd high, single-sided wall quads (rendered = collidable),
# a door gap x -1..1 (z < 3) in the south wall with a lintel above it, flat roof.
def quad_x(x0, x1, y, z0, z1, ny):         # wall in the plane y = const, CCW seen from its normal (0, ny, 0)
    if ny > 0: x0, x1 = x1, x0
    return ((x0, y, z0), (x1, y, z0), (x1, y, z1), (x0, y, z1), (0, ny, 0))
def quad_y(x, y0, y1, z0, z1, nx):         # wall in the plane x = const, normal (nx, 0, 0)
    if nx < 0: y0, y1 = y1, y0
    return ((x, y0, z0), (x, y1, z0), (x, y1, z1), (x, y0, z1), (nx, 0, 0))
hut_quads = [quad_x(-5, 5, 4, 0, 4, 1), quad_y(5, -4, 4, 0, 4, 1), quad_y(-5, -4, 4, 0, 4, -1),
             quad_x(-5, -1, -4, 0, 4, -1), quad_x(1, 5, -4, 0, 4, -1), quad_x(-1, 1, -4, 3, 4, -1),
             ((-5, -4, 4), (5, -4, 4), (5, 4, 4), (-5, 4, 4), (0, 0, 1))]
hut_groups = [((-5, -4, 0), (5, 4, 4), 0x8)]
write("World/wmo/Test/Hut.wmo", wmo_root(hut_groups, "Textures\\Test\\Wall.blp", [], [("Set_$DefaultGlobal", 0, 0)]))
write("World/wmo/Test/Hut_000.wmo", box_group(*hut_groups[0][:2], 0x8, 0, quads=hut_quads))

# Fence doodad (M2 with a collision mesh): 6 x 0.2 x 1.2 box, render mesh = collision mesh.
from m2_writer import m2 as m2_file
fence_lo, fence_hi = (-3.0, -0.1, 0.0), (3.0, 0.1, 1.2)
def box_corners(lo, hi): return [(hi[0] if k & 1 else lo[0], hi[1] if k & 2 else lo[1], hi[2] if k & 4 else lo[2]) for k in range(8)]
fence_v = box_corners(fence_lo, fence_hi)
fence_t = [0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5]
fence_rv = [(p, (255, 0, 0, 0), (0, 0, 0, 0), tuple(1 if p[i] > 0.5 * (fence_lo[i] + fence_hi[i]) else -1 for i in range(3)),
             (0.0, 0.0)) for p in fence_v]
# The collision mesh is the same box finely subdivided (12 x 4 quads per face, shared vertices):
# 400+ triangles, over the converter's default budget, so the simplified path is what gets tested.
cv, ct, vid = [], [], {}
def cvert(p):
    p = tuple(round(c, 6) for c in p)
    if p not in vid: vid[p] = len(cv); cv.append(p)
    return vid[p]
def face(o, u, v, nu=12, nv=4):          # o + a*u + b*v, a, b in [0, 1]; CCW seen along u x v
    for i in range(nu):
        for j in range(nv):
            q = [cvert(tuple(o[k] + (i + di) / nu * u[k] + (j + dj) / nv * v[k] for k in range(3)))
                 for di, dj in ((0, 0), (1, 0), (1, 1), (0, 1))]
            ct.extend([q[0], q[1], q[2], q[0], q[2], q[3]])
(x0, y0, z0), (x1, y1, z1) = fence_lo, fence_hi
X, Y, Z = x1 - x0, y1 - y0, z1 - z0
face((x0, y0, z0), (X, 0, 0), (0, 0, Z)); face((x1, y1, z0), (-X, 0, 0), (0, 0, Z))     # long sides -y / +y
face((x0, y1, z0), (0, -Y, 0), (0, 0, Z)); face((x1, y0, z0), (0, Y, 0), (0, 0, Z))     # ends -x / +x
face((x0, y0, z1), (X, 0, 0), (0, Y, 0)); face((x0, y1, z0), (X, 0, 0), (0, -Y, 0))     # top / bottom
write("World/Test/Fence.m2", m2_file("Fence", fence_rv, fence_t, [(0, 0, len(fence_t))], [(0, 0, 0)],
                                     [(0, "Textures\\Test\\Wall.blp")], [0], [(0, 0)], [(-1, (0, 0, 0), {})],
                                     [(0, 0, 0, 1000, 0.0)], [], coll=(cv, ct)))

# Lamp doodad (stage 5.32 effects): a post, a glow card on top (additive quad with a "glow" texture: becomes a glow sprite,
# out of the mesh) and a flame emitter (20/s, 1 s, a 2x2 atlas).
from m2_writer import blp as blp_file
write("World/Test/LampGlow.blp", blp_file(5))
write("Particles/Test/Flame.blp", blp_file(9))
lamp_lo, lamp_hi = (-0.1, -0.1, 0.0), (0.1, 0.1, 2.0)
lamp_v = [(p, (255, 0, 0, 0), (0, 0, 0, 0), tuple(1 if p[i] > 0.5 * (lamp_lo[i] + lamp_hi[i]) else -1 for i in range(3)), (0.0, 0.0))
          for p in box_corners(lamp_lo, lamp_hi)]
glow_q = [(0.0, -0.3, 1.9), (0.0, 0.3, 1.9), (0.0, 0.3, 2.5), (0.0, -0.3, 2.5)]
lamp_v += [(p, (255, 0, 0, 0), (0, 0, 0, 0), (1, 0, 0), (0.0, 0.0)) for p in glow_q]
lamp_t = list(fence_t) + [8, 9, 10, 8, 10, 11]
write("World/Test/Lamp.m2", m2_file("Lamp", lamp_v, lamp_t, [(0, 0, len(fence_t)), (1, len(fence_t), 6)], [(0, 0, 0), (1, 1, 1)],
                                    [(0, "Textures\\Test\\Wall.blp"), (0, "World\\Test\\LampGlow.blp"), (0, "Particles\\Test\\Flame.blp")],
                                    [0, 1], [(0, 0), (0, 3)], [(-1, (0, 0, 0), {})], [(0, 0, 0, 1000, 0.0)], [],
                                    emitters=[dict(pos=(0.0, 0.0, 2.2), texture=2, blend=4, type=1, rows=2, cols=2, flags=0x40,
                                                   rate=20.0, life=1.0, speed=1.0, gravity=-0.5, scales=(0.3, 0.25, 0.1),
                                                   colors=[(255, 200, 80, 255), (255, 120, 40, 200), (120, 40, 20, 0)])]))

# ------------------------------------------------------------ ADTs
def adt(tx, ty, modf, mddf):
    mtex = b"Textures\\Test\\Wall.blp\0"
    mwmo = b"World\\wmo\\Test\\House.wmo\0World\\wmo\\Test\\Tower.wmo\0World\\wmo\\Test\\Stairs.wmo\0World\\wmo\\Test\\Hut.wmo\0"
    mmdx = b"World\\Test\\Lamp.mdx\0World\\Test\\Fence.mdx\0"
    data = chunk("MVER", struct.pack("<I", 18)) + chunk("MTEX", mtex) + chunk("MMDX", mmdx)
    data += chunk("MMID", struct.pack("<II", 0, len(b"World\\Test\\Lamp.mdx\0")))
    data += chunk("MWMO", mwmo) + chunk("MWID", struct.pack("<II", 0, len(b"World\\wmo\\Test\\House.wmo\0")))
    data += chunk("MDDF", b"".join(mddf)) + chunk("MODF", b"".join(modf))
    for cy in range(16):
        for cx in range(16):
            hdrc = bytearray(128)
            struct.pack_into("<IIII", hdrc, 0, 0, cx, cy, 0)
            struct.pack_into("<II", hdrc, 20, 128, 128 + 8 + 580)    # ofsHeight, ofsNormal (payload-relative)
            struct.pack_into("<fff", hdrc, 104, 0, 0, 80.0)          # height base
            mcvt = chunk("MCVT", struct.pack("<145f", *([0.0] * 145)))
            mcnr = chunk("MCNR", bytes([0, 0, 127] * 145) + bytes(13))
            data += chunk("MCNK", bytes(hdrc) + mcvt + mcnr)
    write(f"World/Maps/Test/Test_{tx}_{ty}.adt", data)

def render_to_adt(x, y, z): return (ZP - y, z, ZP - x)

def modf(name_id, uid, render_pos, rot_deg, doodad_set):
    ax, ay, az = render_to_adt(*render_pos)
    return struct.pack("<II3f3f3f3fHHHH", name_id, uid, ax, ay, az, *rot_deg, ax - 20, ay - 5, az - 20, ax + 20, ay + 25, az + 20,
                       0, doodad_set, 0, 0)

def mddf(name_id, uid, render_pos, rot_deg, scale):
    ax, ay, az = render_to_adt(*render_pos)
    return struct.pack("<II3f3fHH", name_id, uid, ax, ay, az, *rot_deg, int(scale * 1024), 0)

house_pos = (-8900.0, -200.0, 80.0)       # tile 32_48
tower_pos = (-8950.0, -1200.0, 80.0)      # tile 34_48 (outside the region 32..33)
house_rot = (10.0, 30.0, 5.0)             # MODF degrees: (about Y, about Z - 180, about X)
house = modf(0, 777, house_pos, house_rot, 1)
tower = modf(1, 778, tower_pos, (0.0, 0.0, 0.0), 0)
lamp_dup = mddf(0, 4242, (-8890.0, -210.0, 80.0), (0.0, 45.0, 0.0), 1.0)   # listed by both tiles
# Stairs right on the 32_48 / 33_48 border (render y = -533.33): its floors must be clipped into both tiles.
stairs_pos = (-8920.0, -537.0, 80.0)
stairs_rot = (0.0, -90.0, 0.0)           # MODF: about Z = -90 + 180 = 90 deg, so local +x points to +y
stairs = modf(2, 779, stairs_pos, stairs_rot, 0)
# Walls: the hut (WMO, turned 25 degrees), a fence (M2, turned 60 degrees, scale 1.5) and a second fence across
# the 32_48 / 33_48 border (along render y, listed by both tiles like real ADTs do).
hut_pos, hut_rot = (-8800.0, -100.0, 80.0), (0.0, 25.0 - 180.0, 0.0)
hut = modf(3, 780, hut_pos, hut_rot, 0)
fence1_pos, fence1_rot, fence1_scale = (-8780.0, -140.0, 80.0), (0.0, 60.0 - 180.0, 0.0), 1.5
fence2_pos, fence2_rot, fence2_scale = (-8960.0, -533.0, 80.0), (0.0, 90.0 - 180.0, 0.0), 1.0
fence1 = mddf(1, 5001, fence1_pos, fence1_rot, fence1_scale)
fence2 = mddf(1, 5002, fence2_pos, fence2_rot, fence2_scale)
adt(32, 48, [house, stairs, hut], [lamp_dup, fence1, fence2])
adt(33, 48, [house, tower, stairs], [lamp_dup, fence2])

# ------------------------------------------------------------ expectations
def rot(axis, a):
    c, s = math.cos(a), math.sin(a)
    if axis == "x": return [[1, 0, 0], [0, c, -s], [0, s, c]]
    if axis == "y": return [[c, 0, s], [0, 1, 0], [-s, 0, c]]
    return [[c, -s, 0], [s, c, 0], [0, 0, 1]]
def mm(a, b): return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
def mv(a, v): return [sum(a[i][k] * v[k] for k in range(3)) for i in range(3)]
def euler(r):                              # WoWee placementEuler (radians): (about X, about Y, about Z)
    return (math.radians(r[2]), math.radians(r[0]), math.radians(r[1] + 180))
def R_of(e): return mm(mm(rot("z", e[2]), rot("y", e[1])), rot("x", e[0]))
def quat_m(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]

f32 = lambda v: struct.unpack("<f", struct.pack("<f", v))[0]
def placed(render_pos, rot_deg, groups, doodads, sets_used):
    ax, ay, az = (f32(v) for v in render_to_adt(*render_pos))
    P = (ZP - az, ZP - ax, ay)            # what adtToWorld gives back
    e = euler(rot_deg); R = R_of(e)
    out = {"groups": [], "doodads": []}
    for lo, hi, fl in groups:
        c = [(lo[i] + hi[i]) / 2 for i in range(3)]
        p = mv(R, c)
        out["groups"].append({"pos": [P[i] + p[i] for i in range(3)], "rot": list(e)})
    for si in sets_used:
        _, start, count = sets_used[si]
        for path, pos, q, sc in doodads[start:start + count]:
            M = mm(R, quat_m(q))           # rotation part of W * local (scale separate)
            p = mv(R, pos)
            out["doodads"].append({"path": path, "pos": [P[i] + p[i] for i in range(3)], "R": M, "scale": sc})
    return out

exp = {"house": placed(house_pos, house_rot, house_groups, house_doodads, {0: house_sets[0], 1: house_sets[1]}),
       "tower": placed(tower_pos, (0, 0, 0), tower_groups, [], {}),
       "house_group_tris": [12, 12], "tower_group_tris": [12]}
# ---- floor queries: brute force over every walkable collidable triangle (render space)
def world_tris(render_pos, rot_deg, local_tris):
    ax, ay, az = (f32(v) for v in render_to_adt(*render_pos))
    P = (ZP - az, ZP - ax, ay); R = R_of(euler(rot_deg)); out = []
    for t in local_tris:
        w = [[P[i] + mv(R, v)[i] for i in range(3)] for v in t]
        e1 = [w[1][i] - w[0][i] for i in range(3)]; e2 = [w[2][i] - w[0][i] for i in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        l = math.sqrt(sum(x * x for x in n))
        if l > 1e-9 and abs(n[2]) / l >= 0.5: out.append(w)
    return out
def box_tris(lo, hi):                         # the 12 triangles box_group writes (collidable: F_RENDER)
    (x0, y0, z0), (x1, y1, z1) = lo, hi
    q = [((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)), ((x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1)),
         ((x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1)), ((x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)),
         ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)), ((x0, y1, z0), (x1, y1, z0), (x1, y0, z0), (x0, y0, z0))]
    return [t for a, b, c, d in q for t in ((a, b, c), (a, c, d))]
floors = []
for lo, hi, fl in house_groups: floors += world_tris(house_pos, house_rot, box_tris(lo, hi))
floors += world_tris(tower_pos, (0, 0, 0), box_tris(*tower_groups[0][:2]))
floors += world_tris(stairs_pos, stairs_rot, box_tris(*stairs_groups[0][:2]) + [t for t, f in ramp])   # carpet excluded
def floor_at(x, y, zfeet, step=1.5, terrain=80.0):
    zs = []
    for w in floors:
        (ax_, ay_, az_), (bx, by, bz), (cx, cy, cz) = w
        den = (by - cy) * (ax_ - cx) + (cx - bx) * (ay_ - cy)
        if abs(den) < 1e-9: continue
        w0 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / den
        w1 = ((cy - ay_) * (x - cx) + (ax_ - cx) * (y - cy)) / den
        w2 = 1 - w0 - w1
        if min(w0, w1, w2) < -1e-6: continue
        zs.append(w0 * az_ + w1 * bz + w2 * cz)
    cand = [z for z in zs + [terrain] if z <= zfeet + step]
    return max(cand) if cand else min(zs + [terrain])
# A walk up the ramp onto the platform and off its far end: local +x maps to render +y here.
queries = []
z = 80.0
for k in range(0, 101):
    lx = -1.9 + k * 0.2                      # local x from -1.9 to 18.1, never exactly on an edge
    px = stairs_pos[0] + 0.0; py = stairs_pos[1] + lx
    z = floor_at(px, py, z)
    queries.append({"x": px, "y": py, "zfeet_in": None, "z": z})
# step-by-step semantics: each query uses the previous answer as feet height (the test replays that)
exp["walk"] = queries
# Spot checks: inside the house at floor level, and from the roof level.
hx, hy = house_pos[0] + 1.0, house_pos[1] + 1.0
exp["spots"] = [{"x": hx, "y": hy, "zfeet": 80.5, "z": floor_at(hx, hy, 80.5)},
                {"x": hx, "y": hy, "zfeet": 88.5, "z": floor_at(hx, hy, 88.5)},
                {"x": hx, "y": hy, "zfeet": 70.0, "z": floor_at(hx, hy, 70.0)},
                {"x": tower_pos[0], "y": tower_pos[1], "zfeet": 80.0, "z": floor_at(tower_pos[0], tower_pos[1], 80.0)},
                {"x": -8700.0, "y": -300.0, "zfeet": 80.0, "z": 80.0}]

# ---- walls: every wall triangle the converter must keep (render space), and walks with known results
def to_world(render_pos, rot_deg, v, scale=1.0):
    ax, ay, az = (f32(c) for c in render_to_adt(*render_pos))
    P = (ZP - az, ZP - ax, ay); R = R_of(euler(rot_deg))
    w = mv(R, [c * scale for c in v])
    return [P[i] + w[i] for i in range(3)]
def steep(t):
    e1 = [t[1][i] - t[0][i] for i in range(3)]; e2 = [t[2][i] - t[0][i] for i in range(3)]
    n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
    l = math.sqrt(sum(x * x for x in n))
    return l > 1e-9 and abs(n[2]) / l < 0.5
def quad_tris(q): a, b, c, d = q[:4]; return [(a, b, c), (a, c, d)]
walls = []
for lo, hi, fl in house_groups: walls += [[to_world(house_pos, house_rot, v) for v in t] for t in box_tris(lo, hi)]
walls += [[to_world(stairs_pos, stairs_rot, v) for v in t] for t in box_tris(*stairs_groups[0][:2]) + [t for t, f in ramp]]
walls += [[to_world(hut_pos, hut_rot, v) for v in t] for q in hut_quads for t in quad_tris(q)]
fence_tris = [[fence_v[fence_t[k + j]] for j in range(3)] for k in range(0, len(fence_t), 3)]
walls += [[to_world(fence1_pos, fence1_rot, v, fence1_scale) for v in t] for t in fence_tris]
walls += [[to_world(fence2_pos, fence2_rot, v, fence2_scale) for v in t] for t in fence_tris]
exp["walls"] = [t for t in walls if steep(t)]

R_WALK = 0.35
def case(name, pos, rot, scale, start, d, dist, want, zfeet=80.0, step=0.25):
    """start/want in the object's local frame (want computed by hand below), d = local direction."""
    R = R_of(euler(rot))
    s0 = to_world(pos, rot, [c / scale for c in start] + [0.0], scale)
    w0 = to_world(pos, rot, [c / scale for c in want] + [0.0], scale)
    dl = math.hypot(*d); dw = mv(R, [d[0] / dl, d[1] / dl, 0.0])
    n = int(round(dist / step))
    return {"name": name, "x": s0[0], "y": s0[1], "z": zfeet, "dx": dw[0] * step, "dy": dw[1] * step, "steps": n,
            "want_x": w0[0], "want_y": w0[1]}
r = R_WALK
s45 = 5.0 / math.sqrt(2)
fh = 0.1 * fence1_scale                    # fence half thickness
exp["wall_cases"] = [
    case("hut north wall, head-on", hut_pos, hut_rot, 1, (0, 8), (0, -1), 6.0, (0, 4 + r)),
    case("hut north wall, 45 deg slide", hut_pos, hut_rot, 1, (-2, 6), (1, -1), 5.0, (-2 + s45, 4 + r)),
    case("hut door, then north wall inside", hut_pos, hut_rot, 1, (0, -8), (0, 1), 12.0, (0, 4 - r)),
    case("hut south wall beside the door", hut_pos, hut_rot, 1, (2.5, -8), (0, 1), 6.0, (2.5, -4 - r)),
    case("hut east wall from inside", hut_pos, hut_rot, 1, (0, -3), (1, 0), 7.0, (5 - r, -3)),
    case("fence head-on", fence1_pos, fence1_rot, fence1_scale, (0, 3), (0, -1), 5.0, (0, fh + r)),
    case("fence jumped over (feet 1.2 up)", fence1_pos, fence1_rot, fence1_scale, (0, 3), (0, -1), 5.0, (0, -2), zfeet=81.2),
    case("fence end passed", fence1_pos, fence1_rot, fence1_scale, (3 * fence1_scale + r + 0.15, 3), (0, -1), 5.0,
         (3 * fence1_scale + r + 0.15, -2)),
    # fence2: local x = render +y; its local y = render -x. Walk render +x = local -y.
    case("border fence, tile 33 side", fence2_pos, fence2_rot, 1, (-2.0, 5), (0, -1), 8.0, (-2.0, 0.1 + r)),
    case("border fence, tile 32 side", fence2_pos, fence2_rot, 1, (2.0, 5), (0, -1), 8.0, (2.0, 0.1 + r)),
    case("along the border fence", fence2_pos, fence2_rot, 1, (-7, 0.1 + r + 0.1), (1, 0), 14.0, (7, 0.1 + r + 0.1)),
]
# Random walks (property test, checked by check_walls.py): start away from every wall, wander.
import random
rng = random.Random(5875)
centres = [(hut_pos, 9.0), (fence1_pos, 7.0), (fence2_pos, 6.0), (house_pos, 15.0)]
walks = []
for k in range(240):
    (cx, cy, _), rad = centres[k % len(centres)]
    while True:   # never start inside a solid fence (inside a box there is no right side to leave by)
        x, y = cx + rng.uniform(-rad, rad), cy + rng.uniform(-rad, rad)
        ok = True
        for fp, frot, fs in ((fence1_pos, fence1_rot, fence1_scale), (fence2_pos, fence2_rot, fence2_scale)):
            o = to_world(fp, frot, [0, 0, 0]); Rt = R_of(euler(frot))
            lx_ = Rt[0][0] * (x - o[0]) + Rt[1][0] * (y - o[1]); ly_ = Rt[0][1] * (x - o[0]) + Rt[1][1] * (y - o[1])
            if abs(lx_) < 3 * fs + R_WALK and abs(ly_) < 0.1 * fs + R_WALK: ok = False
        if ok: break
    a = rng.uniform(0, 2 * math.pi); moves = []
    for i in range(160):
        if rng.random() < 0.08: a = rng.uniform(0, 2 * math.pi)
        st = rng.choice([0.05, 0.12, 0.25, 0.5, 1.0])   # frame steps from 60 fps walking to a 2 s hitch at run speed
        moves.append([st * math.cos(a), st * math.sin(a)])
    walks.append({"x": x, "y": y, "z": rng.choice([80.0, 80.0, 80.0, 80.4]), "moves": moves})
exp["random_walks"] = walks
exp["walker_radius"] = R_WALK
exp["lamp_fx"] = {"emitters": 1, "glows": 1, "glow_pos": [0.0, 0.0, 2.2], "glow_size": 0.3, "rate": 20.0, "life": 1.0}
json.dump(exp, open(expect_path, "w"), indent=1)
print("synthetic data written to", out)
