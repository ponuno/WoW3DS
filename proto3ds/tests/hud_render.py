"""Rasterises hud_test's quad list (400x240, alpha blending, the real atlas from hud_font.h) over a backdrop."""
import re, sys
from PIL import Image
src = open(sys.argv[1]).read()          # hud_font.h
alpha = [int(v) for v in re.search(r"kHudFontAlpha4\[8192\] = \{(.*?)\};", src, re.S).group(1).replace("\n", "").split(",") if v.strip()]
def A(x, y):
    p = y * 128 + x
    b = alpha[p >> 1]
    return (b >> 4) if p & 1 else (b & 15)
img = Image.new("RGB", (400, 240), (96, 140, 70))
for y in range(120): 
    for x in range(400): img.putpixel((x, y), (140, 180, 230))
px = img.load()
for line in open(sys.argv[2]):
    x0, y0, x1, y1, u0, v0, u1, v1, col, tex = map(int, line.split())
    r, g, b, a = col & 255, (col >> 8) & 255, (col >> 16) & 255, (col >> 24) & 255
    for y in range(max(0, y0), min(240, y1)):
        for x in range(max(0, x0), min(400, x1)):
            al = a
            if tex:
                al = a * (A(u0 + x - x0, v0 + y - y0) * 17) // 255
            if al <= 0: continue
            o = px[x, y]
            px[x, y] = tuple((c * al + oc * (255 - al)) // 255 for c, oc in zip((r, g, b), o))
img.resize((1200, 720), Image.NEAREST).save(sys.argv[3])
