"""Rough synthetic ground for the GRND tests (6.18a): heights of the 145 vertices of every chunk of Test_32_48.adt.
The height is a smooth function of the tile-wide grid position (corners at whole squares, centres at +0.5), so chunk
borders agree; the centres are NOT the mean of their corners (that is the point: the old corner-only field was wrong there)."""
import math

BASE = 80.0                       # MCNK position z
HOLES = {(5, 5): 0x0001, (5, 6): 0x0660, (9, 3): 0x8421}


def height(gx, gy):
    return (4.0 * math.sin(gx * 0.9) + 3.0 * math.cos(gy * 1.3) + 2.0 * math.sin((gx + gy) * 0.37)
            + 1.5 * math.sin(gx * 2.1 + gy * 1.7))


def vertex_grid(cy, cx, idx):
    """tile-wide (row, col) in squares of ADT vertex idx (0..144) of chunk (cx, cy)."""
    r, k = divmod(idx, 17)
    if k < 9:
        return cy * 8 + r, cx * 8 + k
    return cy * 8 + r + 0.5, cx * 8 + (k - 9) + 0.5


def mcvt(cy, cx):
    out = []
    for idx in range(145):
        gx, gy = vertex_grid(cy, cx, idx)
        out.append(height(gx, gy))
    return out
