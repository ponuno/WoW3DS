"""dump_bnet_protos.py — the Battle.net protobuf definitions as compiled into a TrinityCore Forever fork (src/server/proto/**/*.pb.cc
hold each .proto file serialized): writes them as one FileDescriptorSet (for the fake server of the tests) and, with --text, prints
every message (field numbers) and service (method ids). Needs the 'protobuf' Python package.
  dump_bnet_protos.py <TrinityCore dir> <out.desc> [--text]"""
import re, sys, glob
from google.protobuf import descriptor_pb2

def cstr(lit):
    out = bytearray(); i = 0
    while i < len(lit):
        c = lit[i]
        if c == '\\':
            n = lit[i + 1]
            if n in '01234567':
                j = i + 1; v = ''
                while j < len(lit) and len(v) < 3 and lit[j] in '01234567': v += lit[j]; j += 1
                out.append(int(v, 8)); i = j; continue
            if n == 'x':
                j = i + 2; v = ''
                while j < len(lit) and lit[j] in '0123456789abcdefABCDEF': v += lit[j]; j += 1
                out.append(int(v, 16) & 255); i = j; continue
            out.append({'n': 10, 't': 9, 'r': 13, '"': 34, "'": 39, '\\': 92, '?': 63}[n]); i += 2; continue
        out.append(ord(c)); i += 1
    return bytes(out)

fds = descriptor_pb2.FileDescriptorSet()
for f in sorted(glob.glob(sys.argv[1] + '/src/server/proto/**/*.pb.cc', recursive=True)):
    m = re.search(r'InternalAddGeneratedFile\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+),\s*(\d+)\);', open(f).read())
    if not m: continue
    data = b''.join(cstr(l) for l in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))
    fd = fds.file.add(); fd.ParseFromString(data)
open(sys.argv[2], 'wb').write(fds.SerializeToString())
print('%d proto files -> %s' % (len(fds.file), sys.argv[2]))
if '--text' in sys.argv:
    for fd in fds.file:
        for s in fd.service:
            for me in s.method:
                mm = re.search(rb'\x82\xf9\x2b.\x08(.)', me.options.SerializeToString(), re.S)
                print('%s.%s.%s id %s' % (fd.package, s.name, me.name, mm.group(1)[0] if mm else '?'))
