"""Synthetic ADT with texture layers for the ground splat (stage 5.36; no client files needed).

    python3 gen_terrain_layers.py <out Data dir> [rough]

With "rough" the ground is uneven (terrain_rough.py: centres off the mean of their corners) instead of flat (6.18a, GRND tests).

World/Maps/Test/Test_32_48.adt, flat terrain; three chunks in the first row:
    (0,0)  four layers: A, B (alpha 20), C (alpha 255), D (alpha 128)   -> base A + the two strongest overlays C, D
    (0,1)  two layers:  A, B (alpha 255)                                -> A, B, B
    (0,2)  one layer:   A                                               -> A, A, A
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import terrain_rough as rough

out = sys.argv[1]
ROUGH = len(sys.argv) > 2 and sys.argv[2] == "rough"

def chunk(magic, payload):
    return magic[::-1].encode() + struct.pack("<I", len(payload)) + payload

def write(path, data):
    p = os.path.join(out, path.replace("\\", "/"))
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(data)

def blp(path, w, shade):
    pal = b"".join(struct.pack("<BBBB", *((shade * 20 + i // 4) % 256,) * 3, 255) for i in range(256))
    pix = bytes((x + y * shade) % 256 for y in range(w) for x in range(w))
    hdr = b"BLP2" + struct.pack("<IBBBBII", 1, 1, 0, 0, 0, w, w)
    offs = [0] * 16; sizes = [0] * 16
    offs[0] = len(hdr) + 64 + 64 + 1024; sizes[0] = len(pix)
    write(path, hdr + struct.pack("<16I", *offs) + struct.pack("<16I", *sizes) + pal + pix)

names = ["A", "B", "C", "D"]
for i, n in enumerate(names):
    blp(f"Textures/Test/{n}.blp", 16, i + 1)

# chunk -> [(texture index, alpha value or None for the base layer)]
layout = {
    (0, 0): [(0, None), (1, 20), (2, 255), (3, 128)],
    (0, 1): [(0, None), (1, 255)],
    (0, 2): [(0, None)],
}

mtex = b"".join(f"Textures\\Test\\{n}.blp".encode() + b"\0" for n in names)
data = chunk("MVER", struct.pack("<I", 18)) + chunk("MTEX", mtex) + chunk("MMDX", b"") + chunk("MMID", b"")
data += chunk("MWMO", b"") + chunk("MWID", b"") + chunk("MDDF", b"") + chunk("MODF", b"")
for cy in range(16):
    for cx in range(16):
        lay = layout.get((cy, cx), [(0, None)])
        mcly = b""; mcal = b""
        for tex, a in lay:
            if a is None:
                mcly += struct.pack("<IIII", tex, 0, 0, 0)
            else:
                mcly += struct.pack("<IIII", tex, 0x100, len(mcal), 0)       # 8-bit alpha map, 64 x 64
                mcal += bytes([a]) * 4096
        hdrc = bytearray(128)
        struct.pack_into("<IIII", hdrc, 0, 0, cx, cy, len(lay))
        body = chunk("MCVT", struct.pack("<145f", *(rough.mcvt(cy, cx) if ROUGH else [0.0] * 145))) + chunk("MCNR", bytes([0, 0, 127] * 145) + bytes(13))
        ofs_layer = 8 + 128 + len(body)
        body += chunk("MCLY", mcly)
        ofs_alpha = 8 + 128 + len(body)
        body += chunk("MCAL", mcal)
        struct.pack_into("<II", hdrc, 20, 136, 136 + 588)
        struct.pack_into("<I", hdrc, 28, ofs_layer)
        struct.pack_into("<II", hdrc, 36, ofs_alpha, len(mcal))   # offsets from the MCNK start, sizeAlpha without the sub-chunk header
        struct.pack_into("<fff", hdrc, 104, 0, 0, 80.0)   # (z = 80 = rough.BASE)
        # 6.13: holes (a building's footprint) in a few chunks: the coarse LODs must still cover the ground around them
        holes = {(5, 5): 0x0001, (5, 6): 0x0660, (9, 3): 0x8421}.get((cy, cx), 0)
        struct.pack_into("<H", hdrc, 60, holes)
        data += chunk("MCNK", bytes(hdrc) + body)
write("World/Maps/Test/Test_32_48.adt", data)
