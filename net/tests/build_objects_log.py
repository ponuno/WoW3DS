"""Builds a W3DL packet log with known object updates and the positions the
client must compute from them (independent Python math), for objects_test.

    python3 build_objects_log.py out.w3dl out.expect
.expect lines: <time_ms> <guid hex> <x> <y> <z> [orientation|-] [present 0/1]
"""
import math, random, struct, sys, zlib
sys.path.insert(0, __import__("os").path.dirname(__file__))
from stage5 import (SPAWN, CREATURE_GUID, F_FORWARD, F_SPLINE, F_SR, UF_HIGHGUID, create, living_block,
                    position_block, values, out_of_range, update_packet, unit_fields, monster_move, packguid,
                    movement_info)

log = open(sys.argv[1], "wb"); log.write(b"W3DL")
exp = open(sys.argv[2], "w")

PACKETS = []
def pk(ms, op, data):                        # written sorted by time at the end
    PACKETS.append((ms, len(PACKETS), op, data))

def expect(ms, guid, pos, o=None, present=1):
    exp.write(f"{ms} {guid:x} {pos[0]:.4f} {pos[1]:.4f} {pos[2]:.4f} {'-' if o is None else f'{o:.4f}'} {present}\n")

def along(path, t):                          # linear interpolation by length, t in [0,1]
    seg = [math.dist(path[i - 1], path[i]) for i in range(1, len(path))]
    want, acc = min(1.0, t) * sum(seg), 0.0
    for i, L in enumerate(seg):
        if acc + L >= want or i == len(seg) - 1:
            k = 1.0 if L < 1e-6 else min(1.0, (want - acc) / L)
            a, b = path[i], path[i + 1]
            return tuple(a[j] + (b[j] - a[j]) * k for j in range(3)), math.atan2(b[1] - a[1], b[0] - a[0]) % (2 * math.pi)
        acc += L

def quant(mid, p):                           # what packed deltas decode to
    q = [int((mid[i] - p[i]) / 0.25) for i in range(3)]
    return tuple(mid[i] - q[i] * 0.25 for i in range(3))

S = SPAWN
w1, w2, w3, w4, w5 = (CREATURE_GUID(299, k) for k in range(1, 6))
other = 0x77
f32 = lambda v: struct.unpack("<f", struct.pack("<f", v))[0]   # float32 rounding like the wire

blocks = []
for g in (w1, w2, w3, w4):
    blocks.append(create(g, 3, living_block(UF_HIGHGUID, 0, S, 0.0), unit_fields(g, 299, 3, 40, 903)))
# w5 is created already following a spline: 500 ms of 4000 passed
pts = [(S[0] + 10, S[1], S[2]), (S[0] + 10, S[1] + 10, S[2])]
end = (S[0] + 20, S[1] + 10, S[2])
blocks.append(create(w5, 3, living_block(UF_HIGHGUID, F_FORWARD | F_SPLINE, S, 0.0,
                                         spline=(0x100, None, 500, 4000, pts, end)), unit_fields(w5, 299, 3, 40, 903)))
of = unit_fields(other, 0, 9, 100, 50); of[2] = 0x19
blocks.append(create(other, 4, living_block(0, 0, (S[0] - 5, S[1] - 5, S[2]), 0.5), of))
pk(0, 0x0A9, update_packet(blocks))
for g in (w1, w2, w3, w4):
    expect(0, g, S)

# w1: linear monster move with two packed intermediate points, 4 s
dest = (S[0] + 30, S[1] + 20, S[2] + 2)
mids = [(S[0] + 7.3, S[1] + 2.1, S[2] + 0.4), (S[0] + 15.8, S[1] + 11.6, S[2] + 1.1)]
pk(1000, 0x0DD, monster_move(w1, S, dest, mids, 4000))
mid = tuple((f32(S[i]) + f32(dest[i])) / 2 for i in range(3))
path1 = [tuple(map(f32, S))] + [quant(mid, m) for m in mids] + [tuple(map(f32, dest))]
for ms in (1000, 2000, 3500, 5000, 7000):
    p, o = along(path1, (ms - 1000) / 4000)
    expect(ms, w1, p, o if ms < 5000 else None)

# w2: uncompressed (flying) path, 2 s
fly = [(S[0], S[1] + 40, S[2] + 10), (S[0] + 5, S[1] + 45, S[2] + 12)]
pk(1000, 0x0DD, monster_move(w2, S, None, None, 2000, flags=0x200, uncompressed_pts=fly))
for ms in (1500, 2600, 3200):
    expect(ms, w2, along([S] + fly, (ms - 1000) / 2000)[0])

# w3: starts moving, then a STOP at a new spot
pk(1000, 0x0DD, monster_move(w3, S, (S[0] + 50, S[1], S[2]), [], 5000))
pk(2000, 0x0DD, monster_move(w3, (S[0] + 10, S[1], S[2]), None, None, 0, kind=1))
expect(3000, w3, (S[0] + 10, S[1], S[2]))

# w4: move with a final facing angle
pk(1000, 0x0DD, monster_move(w4, S, (S[0], S[1] + 8, S[2]), [], 1000, kind=4, facing=2.25))
expect(1500, w4, (S[0], S[1] + 4, S[2]), math.pi / 2)
expect(2500, w4, (S[0], S[1] + 8, S[2]), 2.25)

# w5: rest of the spline (3500 ms) from its reported position
path5 = [S] + pts + [end]
for ms in (0, 1750, 3500, 5000):
    expect(ms, w5, along(path5, ms / 3500)[0])

# other player: START_FORWARD at t=1000 facing 0.5, HEARTBEAT at 1500, dead reckoning, STOP at 2500
op = (S[0] - 5, S[1] - 5, S[2])
pk(1000, 0x0B5, packguid(other) + movement_info(F_FORWARD, 1000, *op, 0.5))
expect(1400, other, (op[0] + math.cos(0.5) * 2.8, op[1] + math.sin(0.5) * 2.8, op[2]), 0.5)
op2 = (op[0] + math.cos(0.5) * 3.5, op[1] + math.sin(0.5) * 3.5, op[2])
pk(1500, 0x0EE, packguid(other) + movement_info(F_FORWARD | F_SR, 1500, *op2, 0.5))   # forward + strafe right
a = 0.5 + math.atan2(-1, 1)
expect(1700, other, (op2[0] + math.cos(a) * 1.4, op2[1] + math.sin(a) * 1.4, op2[2]))
pk(2500, 0x0B7, packguid(other) + movement_info(0, 2500, S[0], S[1] + 1, S[2], 1.0))
expect(4000, other, (S[0], S[1] + 1, S[2]), 1.0)

# a gameobject, then destroyed; a unit sent out of range; health via VALUES (compressed)
go = 0xF110000000000000 | (55 << 24) | 1
pk(3000, 0x0A9, update_packet([create(go, 5, position_block(UF_HIGHGUID, (S[0] + 1, S[1] + 2, S[2] + 3), 0.7),
                                      {0: 1, 2: 0x21, 3: 55, 8: 200})]))
expect(3000, go, (S[0] + 1, S[1] + 2, S[2] + 3), 0.7)
raw = update_packet([values(w4, {22: 7}), out_of_range([w3])])
pk(3100, 0x1F6, struct.pack("<I", len(raw)) + zlib.compress(raw, 9))
pk(3200, 0x0AA, struct.pack("<Q", go))
expect(5000, w3, (0, 0, 0), None, 0)
expect(5000, go, (0, 0, 0), None, 0)
exp.write(f"HEALTH {w4:x} 7\n")
# GUID reused with another object type must not keep the old fields
pk(3300, 0x0A9, update_packet([create(w2, 5, position_block(0, (S[0], S[1], S[2]), 0.0), {2: 0x21, 3: 9})]))
exp.write(f"TYPE {w2:x} 5\n")
exp.write(f"HEALTH {w2:x} 0\n")
# a truncated CREATE: must be rejected without disturbing anything
bad = update_packet([create(0xF130000000009999, 3, living_block(0, 0, S, 0.0), unit_fields(1, 1, 1, 1, 1))])
pk(3400, 0x0A9, bad[:-7])
exp.write("FAILED 1\n")
exp.write(f"ABSENT {0xF130000000009999:x}\n")

for ms, _, op, data in sorted(PACKETS):
    log.write(struct.pack("<IBHI", ms, 0, op, len(data)) + data)
