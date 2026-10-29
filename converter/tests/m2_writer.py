"""Minimal vanilla (v256) M2 writer + palettised BLP2 for the synthetic converter tests
(gen_creature_data.py: animated creatures; gen_wmo_data.py: a static doodad with collision)."""
import struct

def blp(seed):
    W = H = 16
    pal = b"".join(struct.pack("<BBBB", (i * seed) % 256, (i * 3) % 256, 255 - i, 255) for i in range(256))
    pix = bytes((x * 7 + y * seed) % 256 for y in range(H) for x in range(W))
    hdr = b"BLP2" + struct.pack("<IBBBBII", 1, 1, 0, 0, 0, W, H)
    offs = [0] * 16; sizes = [0] * 16; offs[0] = len(hdr) + 128 + 1024; sizes[0] = len(pix)
    return hdr + struct.pack("<16I", *offs) + struct.pack("<16I", *sizes) + pal + pix

# ------------------------------------------------------------ vanilla M2 writer
class Blob:
    def __init__(self, header_size): self.b = bytearray(header_size)
    def add(self, data):
        while len(self.b) % 16: self.b.append(0)
        off = len(self.b); self.b += data; return off

def m2(name, verts, tris, submeshes, batches, textures, tex_lookup, render_flags, bones, seqs, global_seqs, coll=None,
       attachments=None, cameras=None, emitters=None):
    """verts: (pos, weights4, idx4, normal, uv); submeshes: (id, indexStart, indexCount); batches: (submesh, material,
    texCombo); textures: (type, filename); bones: (parent, pivot, tracks{t,r,s}); a track: (globalSeq,
    [(seq or None, [(time, value), ...])])  times are global-timeline ms; seqs: (id, var, start, end, speed).
    coll: optional collision mesh (bounding vertices [(x, y, z)], bounding triangle indices [i, ...]).
    attachments: optional [(id, bone, (x, y, z))] (vanilla 48-byte records).
    cameras: optional [(type, fov, far, near, (position), (target))] (vanilla 124-byte records, no animation).
    emitters: optional particle emitters (vanilla 504-byte records), dicts with pos, texture, blend, type, rows, cols,
    flags, rate, life, speed, gravity, ..., colors [(r, g, b, a) x3], scales [x3], mid."""
    HDR = 0x2C + 8 + 4 * 2 * 9 + 8 + 4 * 2 * 6 + 56 + 4 * 2 * 11
    B = Blob(HDR)
    name_off = B.add(name.encode() + b"\0")
    gs_off = B.add(struct.pack(f"<{len(global_seqs)}I", *global_seqs)) if global_seqs else 0
    seq_data = b""
    for sid, var, st, en, spd in seqs:
        seq_data += struct.pack("<HHIIfIhHIII6ffhH", sid, var, st, en, spd, 0, 0, 0, 0, 0, 150, -1, -1, 0, 1, 1, 2, 2.5, -1, 0)
    seq_off = B.add(seq_data)
    anim_lookup_off = B.add(struct.pack("<h", 0))

    def track(tr, kind):   # kind: 'v3' or 'q'
        if tr is None: return struct.pack("<HhIIIIII", 0, -1, 0, 0, 0, 0, 0, 0)
        gseq, per_seq = tr
        ranges, ts, keys = [], [], b""
        for _, keylist in per_seq:
            start = len(ts)
            for t, v in keylist:
                ts.append(t)
                keys += struct.pack("<4f", *v) if kind == "q" else struct.pack("<3f", *v)
            ranges.append((start, len(ts)))
        r_off = B.add(b"".join(struct.pack("<II", a, b) for a, b in ranges)) if gseq < 0 else 0
        t_off = B.add(struct.pack(f"<{len(ts)}I", *ts))
        k_off = B.add(keys)
        return struct.pack("<HhIIIIII", 1, gseq, len(ranges) if gseq < 0 else 0, r_off, len(ts), t_off, len(ts), k_off)

    bone_data = b""
    for parent, pivot, tr in bones:
        bone_data += struct.pack("<iIhH", -1, 0, parent, 0) + track(tr.get("t"), "v3") + track(tr.get("r"), "q") + \
                     track(tr.get("s"), "v3") + struct.pack("<3f", *pivot)
    bones_off = B.add(bone_data)
    v_off = B.add(b"".join(struct.pack("<3f4B4B3f4f", *p, *w, *i, *n, uv[0], uv[1], 0, 0) for p, w, i, n, uv in verts))
    tex_names = []
    tex_data = b""
    for ttype, fn in textures:
        if fn:
            no = B.add(fn.encode() + b"\0"); tex_data += struct.pack("<IIII", ttype, 0, len(fn) + 1, no)
        else:
            tex_data += struct.pack("<IIII", ttype, 0, 0, 0)
    tex_off = B.add(tex_data)
    rf_off = B.add(b"".join(struct.pack("<HH", f, bl) for f, bl in render_flags))
    tl_off = B.add(struct.pack(f"<{len(tex_lookup)}H", *tex_lookup))
    bl_off = B.add(struct.pack("<H", 0))
    # embedded view
    lookup = list(range(len(verts)))
    li_off = B.add(struct.pack(f"<{len(lookup)}H", *lookup))
    tri_off = B.add(struct.pack(f"<{len(tris)}H", *tris))
    sm_off = B.add(b"".join(struct.pack("<10H3f", sid, 0, 0, len(verts), ist, icn, 0, 0, 0, 0, 0, 0, 0)
                             for sid, ist, icn in submeshes))
    ba_off = B.add(b"".join(struct.pack("<BbHHHHHHHHHHH", 0, 0, 0, sm, 0, 0xFFFF, mat, 0, 1, tc, 0, 0, 0)
                             for sm, mat, tc in batches))
    cv_off = ct_off = 0
    if coll:
        cverts, ctris = coll
        cv_off = B.add(b"".join(struct.pack("<3f", *v) for v in cverts))
        ct_off = B.add(struct.pack(f"<{len(ctris)}H", *ctris))
    at_off = 0
    if attachments:
        at_off = B.add(b"".join(struct.pack("<IHH3f", aid, bone, 0, *pos) + bytes(28) for aid, bone, pos in attachments))
    cam_off = 0
    if cameras:
        empty = struct.pack("<HhIIIIII", 0, -1, 0, 0, 0, 0, 0, 0)
        cam_off = B.add(b"".join(struct.pack("<Ifff", ct, fov, far, near) + empty + struct.pack("<3f", *pos) + empty +
                                 struct.pack("<3f", *tgt) + empty for ct, fov, far, near, pos, tgt in cameras))
    em_off = 0
    if emitters:
        def ftrack(v):   # a float track with one key in sequence 0
            r_off = B.add(struct.pack("<II", 0, 1)); t_off = B.add(struct.pack("<I", 0)); k_off = B.add(struct.pack("<f", v))
            return struct.pack("<HhIIIIII", 1, -1, 1, r_off, 1, t_off, 1, k_off)
        recs = b""
        for e in emitters:
            r = struct.pack("<iI3fHH", -1, e.get("flags", 0), *e["pos"], e.get("bone", 0), e.get("texture", 0))
            r += bytes(16)                                                 # model / child emitter file names
            r += struct.pack("<BBBBhhHH", e.get("blend", 4), e.get("type", 1), 0, 0, 0, 0, e.get("rows", 1), e.get("cols", 1))
            for k in ("speed", "speedVar", "vRange", "hRange", "gravity", "life", "rate", "areaL", "areaW", "decel"):
                r += ftrack(e.get(k, 0.0))
            r += struct.pack("<f", e.get("mid", 0.5))
            for c in e.get("colors", [(255, 255, 255, 255)] * 3):
                r += struct.pack("<I", c[2] | c[1] << 8 | c[0] << 16 | c[3] << 24)   # BGRA
            r += struct.pack("<3f", *e.get("scales", (1.0, 1.0, 1.0)))
            r += bytes(504 - len(r))
            assert len(r) == 504
            recs += r
        em_off = B.add(recs)
    view_off = B.add(struct.pack("<11I", len(lookup), li_off, len(tris), tri_off, 0, 0, len(submeshes), sm_off,
                                 len(batches), ba_off, 0))
    h = struct.pack("<4sIIIIIIIIII", b"MD20", 256, len(name) + 1, name_off, 0, len(global_seqs), gs_off,
                    len(seqs), seq_off, 1, anim_lookup_off)
    h += struct.pack("<II", 0, 0)
    h += struct.pack("<18I", len(bones), bones_off, 0, 0, len(verts), v_off, 1, view_off, 0, 0,
                     len(textures), tex_off, 0, 0, 0, 0, 0, 0)
    h += struct.pack("<II", 0, 0)
    h += struct.pack("<12I", len(render_flags), rf_off, 1, bl_off, len(tex_lookup), tl_off, 0, 0, 0, 0, 0, 0)
    h += struct.pack("<14f", -1, -1, 0, 1, 1, 2, 2, -1, -1, 0, 1, 1, 2, 2)
    # nBoundingTriangles (index count), ofs, nBoundingVertices, ofs, nBoundingNormals, ofs, then 16 more zeros
    h += struct.pack("<22I", len(coll[1]) if coll else 0, ct_off, len(coll[0]) if coll else 0, cv_off, 0, 0,
                     len(attachments) if attachments else 0, at_off, 0, 0, 0, 0, 0, 0,
                     len(cameras) if cameras else 0, cam_off, 0, 0, 0, 0, len(emitters) if emitters else 0, em_off)
    assert len(h) == HDR, (len(h), HDR)
    B.b[0:HDR] = h
    return bytes(B.b)


def blp_rgba(w, h, pixel):
    """BLP2 palettised with 8-bit alpha: pixel(x, y) -> (r, g, b, a), at most 256 distinct colours."""
    pal, idx, alpha = [], {}, []
    pix = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b, a = pixel(x, y)
            if (r, g, b) not in idx:
                idx[(r, g, b)] = len(pal); pal.append((r, g, b))
            pix.append(idx[(r, g, b)]); alpha.append(a)
    assert len(pal) <= 256
    palb = b"".join(struct.pack("<BBBB", b_, g_, r_, 255) for r_, g_, b_ in pal) + bytes(4 * (256 - len(pal)))
    data = bytes(pix) + bytes(alpha)
    hdr = b"BLP2" + struct.pack("<IBBBBII", 1, 1, 8, 8, 0, w, h)   # type 1, palette, alpha depth 8, no mips
    offs = [0] * 16; sizes = [0] * 16; offs[0] = len(hdr) + 128 + 1024; sizes[0] = len(data)
    return hdr + struct.pack("<16I", *offs) + struct.pack("<16I", *sizes) + palb + data
