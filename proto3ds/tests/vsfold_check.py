"""vsfold_check — 6.70: runs the old and the new model vertex shader (.shbin from picasso) on the cases of vsfold_cases with a
small PICA200 interpreter (format-1 ALU ops the shaders use) and compares position, texcoord0 and colour.
  vsfold_check.py old.shbin new.shbin cases.txt"""
import struct, sys, math

def f24(w):
    w &= 0xFFFFFF
    if w & 0x7FFFFF == 0: return 0.0
    s = -1.0 if w >> 23 else 1.0
    e = (w >> 16) & 0x7F
    m = w & 0xFFFF
    return s * (1 + m / 65536.0) * 2.0 ** (e - 63)

class Shader:
    def __init__(self, path):
        b = open(path, 'rb').read()
        n = struct.unpack_from('<I', b, 4)[0]
        dvlp = 8 + 4 * n
        dvle = struct.unpack_from('<I', b, 8)[0]
        co, cs, oo, oc = struct.unpack_from('<IIII', b, dvlp + 8)
        self.code = list(struct.unpack_from('<%dI' % cs, b, dvlp + co))
        self.desc = [struct.unpack_from('<I', b, dvlp + oo + 8 * i)[0] for i in range(oc)]
        hdr = struct.unpack_from('<16I', b, dvle)
        self.main = hdr[2]
        cto, ctc = hdr[6], hdr[7]
        omo, omc = hdr[10], hdr[11]
        uto, utc = hdr[12], hdr[13]
        sto = hdr[14]
        self.consts = {}
        for i in range(ctc):
            t, rid, a, bb, c, d = struct.unpack_from('<HHIIII', b, dvle + cto + 20 * i)
            if t == 2: self.consts[0x20 + rid] = [f24(a), f24(bb), f24(c), f24(d)]
        self.outs = {}
        for i in range(omc):
            t, rid, mask = struct.unpack_from('<HHH', b, dvle + omo + 8 * i)
            self.outs[rid] = t   # 0 position, 2 colour, 3 texcoord0
        self.unif = {}
        for i in range(utc):
            so, r0, r1 = struct.unpack_from('<IHH', b, dvle + uto + 8 * i)
            name = b[dvle + sto + so:].split(b'\0')[0].decode()
            if 0x10 <= r0 <= 0x6F: self.unif[name] = 0x20 + (r0 - 0x10)

    def run(self, uniforms, inputs):
        reg = {}
        for name, vals in uniforms.items():
            if name in self.unif:
                for k, v in enumerate(vals): reg[self.unif[name] + k] = list(v)
        for k, v in self.consts.items(): reg[k] = list(v)
        for k, v in inputs.items(): reg[k] = list(v)
        out = {}
        pc = self.main
        while True:
            w = self.code[pc]; pc += 1
            op = w >> 26
            if op == 0x22: break
            dst = (w >> 21) & 0x1F
            s1 = (w >> 12) & 0x7F
            s2 = (w >> 7) & 0x1F
            d = self.desc[w & 0x7F]
            mask = d & 0xF
            def src(r, neg, swz):
                v = reg.get(r, [0.0] * 4)
                sel = [(swz >> (6 - 2 * i)) & 3 for i in range(4)]
                v = [v[s] for s in sel]
                return [-x for x in v] if neg else v
            a = src(s1, (d >> 4) & 1, (d >> 5) & 0xFF)
            c = src(s2, (d >> 13) & 1, (d >> 14) & 0xFF)
            if op == 0x00: r = [a[i] + c[i] for i in range(4)]
            elif op == 0x01: r = [sum(a[i] * c[i] for i in range(3))] * 4
            elif op == 0x02: r = [sum(a[i] * c[i] for i in range(4))] * 4
            elif op == 0x08: r = [a[i] * c[i] for i in range(4)]
            elif op == 0x0C: r = [max(a[i], c[i]) for i in range(4)]
            elif op == 0x0D: r = [min(a[i], c[i]) for i in range(4)]
            elif op == 0x0F: r = [1.0 / math.sqrt(a[0]) if a[0] > 0 else float('inf')] * 4
            elif op == 0x13: r = a
            else: raise SystemExit('opcode %#x not handled' % op)
            tgt = out.setdefault(dst, [float('nan')] * 4) if dst < 0x10 else reg.setdefault(dst, [0.0] * 4)
            for i in range(4):
                if mask & (8 >> i): tgt[i] = r[i]
        return {self.outs[k]: v for k, v in out.items() if k in self.outs}

def cases(path):
    cur = None
    for line in open(path):
        p = line.split()
        if p[0] == 'case':
            if cur: yield cur
            cur = {}
            continue
        cur[p[0]] = [float(x) for x in p[1:]]
    if cur: yield cur

old, new = Shader(sys.argv[1]), Shader(sys.argv[2])
fails = n = 0
worst = 0.0
for c in cases(sys.argv[3]):
    m = lambda k: [c['%s%d' % (k, i)] for i in range(4)]
    ins = {0: c['v0'], 1: c['v1'], 2: c['v2']}
    uo = {'projection': m('projection'), 'modelView': m('modelView'), 'posScale': [c['posScale']], 'posOffset': [c['posOffset']],
          'lightDir': [c['lightDir']], 'lightParams': [c['lightParams']], 'fogParams': [c['fogParams']],
          'uvRowA': [c['uvRowA']], 'uvRowB': [c['uvRowB']]}
    un = {'mvp': m('mvp'), 'modelView': m('modelViewF'), 'lightDir': [c['lightDirF']], 'lightParams': [c['lightParams']],
          'fogParams': [c['fogParams']], 'uvRowA': [c['uvRowAF']], 'uvRowB': [c['uvRowBF']]}
    a, b = old.run(uo, ins), new.run(un, ins)
    for kind in (0, 2, 3):
        for i in range(4):
            x, y = a[kind][i], b[kind][i]
            err = abs(x - y) / max(1.0, abs(x))
            worst = max(worst, err)
            n += 1
            if not (err < 2e-4): fails += 1; print('FAIL kind', kind, i, x, y)
print('VSFOLD: %d values, %d differ, worst relative %.2g -> %s' % (n, fails, worst, 'FAIL' if fails else 'PASS'))
sys.exit(1 if fails else 0)
