"""6.43: a tiny vanilla M2 for m2_ranges_test: one bone; Stand (timeline 0..1000) with two rotation keys, KneelLoop (2000..3000) with
ONE key (a held pose), Walk (4000..5000) with three translation keys of which the first is 100 ms after the sequence start.
Usage: gen_m2_ranges.py <out.m2>"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m2_writer import m2

verts = [((0, 0, 0), (255, 0, 0, 0), (0, 0, 0, 0), (0, 0, 1), (0, 0)), ((1, 0, 0), (255, 0, 0, 0), (0, 0, 0, 0), (0, 0, 1), (1, 0)),
         ((0, 1, 0), (255, 0, 0, 0), (0, 0, 0, 0), (0, 0, 1), (0, 1))]
q0, q90 = (0, 0, 0, 1), (0, 0, 0.70710678, 0.70710678)
rot = (-1, [(0, [(0, q0), (1000, q90)]),          # Stand
            (1, [(2000, q90)]),                   # KneelLoop: one key - the held pose
            (2, [(4000, q0)])])                   # Walk
tr = (-1, [(0, [(0, (0, 0, 0))]),
           (1, [(2000, (0, 0, -0.5))]),
           (2, [(4100, (0, 0, 0)), (4500, (1, 0, 0)), (5000, (2, 0, 0))])])
data = m2("RangeTest", verts, [0, 1, 2], [(0, 0, 3)], [(0, 0, 0)], [(2, "")], [0], [(0, 0)],
          [(-1, (0, 0, 0), {"r": rot, "t": tr})],
          [(0, 0, 0, 1000, 0.0), (115, 0, 2000, 3000, 0.0), (4, 0, 4000, 5000, 2.5)], [])
open(sys.argv[1], "wb").write(data)
