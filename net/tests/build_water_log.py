"""Stage 5.26: a W3DL log of the liquid packets (mirror timers, environmental damage, fishing) for water_test.

    python3 build_water_log.py out.w3dl out.expect
.expect lines (checked by water_test after replaying the log up to the time given):
  M <ms> <type> <active 0/1> <ms left>        mirror timer state (MirrorTimer::valueAt)
  B <ms> <biting 0/1>                          GameSession::fishBiting
  S <ms> <x> <y> <z> <verb|->                  softInteract at the position (facing +x): its verb, '-' = nothing
  C <text>                                     a chat line that must exist at the end
  H <damage>                                   a floating number on us (MeleeHit with victim = us)
"""
import struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from stage5 import UF_SELF, UF_HIGHGUID, living_block, position_block, create, update_packet, unit_fields, SPAWN

log = open(sys.argv[1], "wb"); log.write(b"W3DL")
exp = open(sys.argv[2], "w")
P = []
def pk(ms, op, data): P.append((ms, len(P), op, data))
def e(line): exp.write(line + "\n")

ME, OTHER = 0x10, 0x11
BOB = 0xF110000000000000 | (35591 << 24) | 7          # our bobber, 20 yd out
BOB2 = 0xF110000000000000 | (35591 << 24) | 8         # somebody else's, right next to us
S = SPAWN
me_f = unit_fields(ME, 0, 10, 300, 49); me_f[2] = 0x19
ot_f = unit_fields(OTHER, 0, 10, 300, 49); ot_f[2] = 0x19
def bobber(guid, owner, pos):
    return create(guid, 5, position_block(UF_HIGHGUID, pos, 0.0),
                  {0: guid & 0xFFFFFFFF, 1: guid >> 32, 2: 0x21, 3: 35591, 6: owner & 0xFFFFFFFF, 7: owner >> 32, 8: 668, 21: 17})
pk(0, 0x0A9, update_packet([create(ME, 4, living_block(UF_SELF, 0, S, 0.0), me_f),
                            create(OTHER, 4, living_block(0, 0, (S[0] + 2, S[1], S[2]), 0.0), ot_f),
                            bobber(BOB, ME, (S[0] + 20, S[1], S[2] - 1)), bobber(BOB2, OTHER, (S[0] + 3, S[1] + 1, S[2] - 1))]))
mirror = lambda t, v, mx, sc, paused=0, spell=0: struct.pack("<IIIiBI", t, v, mx, sc, paused, spell)
# breath: 60 s running down
pk(100, 0x1D9, mirror(1, 60000, 60000, -1))
e("M 100 1 1 60000"); e("M 5100 1 1 55000")
pk(6000, 0x1DA, struct.pack("<IB", 1, 1))                 # paused at 54100
e("M 9000 1 1 54100")
pk(8000, 0x1D9, mirror(0, 30000, 60000, -1))              # fatigue: half gone already
e("M 8000 0 1 30000"); e("M 18000 0 1 20000"); e("M 49000 0 1 0")
pk(9200, 0x1DB, struct.pack("<I", 1))                     # breath stopped
e("M 9300 1 0 0")
pk(9400, 0x1D9, mirror(1, 20000, 60000, 10))              # refilling ten times faster
e("M 10400 1 1 30000"); e("M 20000 1 1 60000")
pk(9450, 0x1D9, mirror(7, 1, 1, -1))                      # an unknown timer type: ignored
pk(9460, 0x1D9, b"\x01\x00")                              # truncated: ignored
# drowning and lava
pk(9500, 0x1FC, struct.pack("<QBIII", ME, 1, 37, 0, 0))
pk(9600, 0x1FC, struct.pack("<QBIII", ME, 3, 120, 20, 0))
pk(9700, 0x1FC, struct.pack("<QBIII", OTHER, 1, 11, 0, 0))   # not in our party: no line
e("C You are drowning and lose 37 health."); e("C You lose 120 health for swimming in lava. (20 absorbed)")
e("H 37"); e("H 120")
# fishing: our bobber is far (20 yd) - "Reel in" only when nothing else is near; a bite makes it the interact target
e(f"S 9900 {S[0]} {S[1]} {S[2]} Reel in")
e("B 9900 0")
pk(10000, 0x0B3, struct.pack("<QI", BOB2, 0))             # the other angler's bite: not ours
e("B 10050 0")
pk(10100, 0x0B3, struct.pack("<QI", BOB, 0))
e("B 10200 1"); e(f"S 10200 {S[0]} {S[1]} {S[2]} Catch")
e(f"S 10200 {S[0] - 30} {S[1]} {S[2]} -")                 # 50 yd away: too far
e("B 15200 0")
pk(17000, 0x1C9, b""); pk(17100, 0x1C8, b"")
e("C Your fish got away!"); e("C No fish are hooked.")
for ms, _, op, data in sorted(P):
    log.write(struct.pack("<IBHI", ms, 0, op, len(data)) + data)
