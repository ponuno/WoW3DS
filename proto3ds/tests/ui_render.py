"""Renders a bottom-screen console dump (VT escapes as libctru's console understands them) to a PNG:
40x30 cells of 8x8 pixels, ESC[r;cH, ESC[2J, ESC[0/1/30-37/90-97/38;5;Nm. Used by run_ui.sh."""
import re, sys
from PIL import Image, ImageDraw, ImageFont

BASE = [(0, 0, 0), (205, 0, 0), (0, 205, 0), (205, 205, 0), (0, 0, 238), (205, 0, 205), (0, 205, 205), (229, 229, 229)]
BRIGHT = [(127, 127, 127), (255, 0, 0), (0, 255, 0), (255, 255, 0), (92, 92, 255), (255, 0, 255), (0, 255, 255), (255, 255, 255)]
def c256(n):
    if n < 8: return BASE[n]
    if n < 16: return BRIGHT[n - 8]
    if n < 232:
        n -= 16; cube = [0, 95, 135, 175, 215, 255]
        return (cube[n // 36], cube[n // 6 % 6], cube[n % 6])
    g = 8 + (n - 232) * 10
    return (g, g, g)

def render(src, out):
    s = open(src, encoding="latin-1").read()
    grid = [[(" ", BASE[7])] * 40 for _ in range(30)]
    r = c = 0; fg = BASE[7]; bold = False; base = 7
    i = 0
    while i < len(s):
        if s[i] == "\x1b":
            m = re.match(r"\x1b\[([0-9;]*)([A-Za-z])", s[i:])
            if not m: i += 1; continue
            args = [int(a) for a in m.group(1).split(";") if a]
            cmd = m.group(2)
            if cmd == "H": r = (args[0] if args else 1) - 1; c = (args[1] if len(args) > 1 else 1) - 1
            elif cmd == "J": grid = [[(" ", BASE[7])] * 40 for _ in range(30)]; r = c = 0
            elif cmd == "m":
                k = 0
                if not args: args = [0]
                while k < len(args):
                    a = args[k]
                    if a == 0: fg = BASE[7]; bold = False; base = 7
                    elif a == 1: bold = True; fg = BRIGHT[base] if base < 8 else fg
                    elif 30 <= a <= 37: base = a - 30; fg = BRIGHT[base] if bold else BASE[base]
                    elif 90 <= a <= 97: base = 8; fg = BRIGHT[a - 90]
                    elif a == 38 and k + 2 < len(args) and args[k + 1] == 5: fg = c256(args[k + 2]); base = 8; k += 2
                    k += 1
            i += len(m.group(0)); continue
        ch = s[i]
        if ch == "\n": r += 1; c = 0
        else:
            if 0 <= r < 30 and 0 <= c < 40: grid[r][c] = (ch, fg)
            c += 1
            if c >= 40: c = 0; r += 1
        i += 1
    S = 3
    img = Image.new("RGB", (320 * S, 240 * S), (0, 0, 0))
    d = ImageDraw.Draw(img)
    font = ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", 8 * S + 1)
    for y in range(30):
        for x in range(40):
            ch, col = grid[y][x]
            if ch != " ": d.text((x * 8 * S, y * 8 * S - 2), ch, fill=col, font=font)
    img.save(out)

for src in sys.argv[1:]:
    render(src, src[:-4] + ".png")
