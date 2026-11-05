"""gen_forever_data.py — synthetic modern client files for forever_data_test: a WDC5 table (inline strings, a bit-packed field,
a pallet field, a common field, an ID list), a WDT with MAID FileDataIDs, an ADT obj0 with MDDF / MODF by FileDataID and an
M2 with MD21 / SFID / TXID / AFID.  gen_forever_data.py <outdir>"""
import struct, sys, os
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
def chunkf(cid, data): return cid.encode() + struct.pack('<I', len(data)) + data   # M2: ids in reading order
def chunk(cid, data): return cid[::-1].encode() + struct.pack('<I', len(data)) + data

# ---- WDC5: 3 records (ids 0, 1, 530), fields: Directory, MapName (strings), InstanceType (3 bits), Expansion (pallet), Flags (common)
names = [('Azeroth', 'Eastern Kingdoms'), ('Kalimdor', 'Kalimdor'), ('Expansion01', 'Outland')]
ids = [0, 1, 530]
record_size = 9
fields = [  # offset_bits, size_bits, extra_size, storage, v1, v2, v3
    (0, 32, 0, 0, 0, 0, 0), (32, 32, 0, 0, 0, 0, 0), (64, 3, 0, 1, 0, 3, 0), (67, 2, 12, 3, 0, 2, 0), (69, 0, 8, 2, 5, 0, 0)]
pallet = struct.pack('<III', 10, 20, 30)
common = struct.pack('<II', 1, 99)   # id 1 -> 99, the others the default 5
hdr_len = 4 + 4 + 128 + 16 * 4 + 2 * 2
sec_off = hdr_len + 40 + 5 * 4 + 5 * 24 + len(pallet) + len(common)
recs = bytearray(record_size * 3)
strings = bytearray(b'\0')
str_base = sec_off + len(recs)
for i, (d, n) in enumerate(names):
    for f, s in ((0, d), (1, n)):
        pos = len(strings); strings += s.encode() + b'\0'
        field_abs = sec_off + i * record_size + fields[f][0] // 8
        struct.pack_into('<I', recs, i * record_size + fields[f][0] // 8, (str_base + pos) - field_abs)
    v = (i + 1) | ((2 - i) << 3)   # instance type i+1 (3 bits at 64), pallet index 2-i (2 bits at 67)
    recs[i * record_size + 8] = v
idlist = struct.pack('<III', *ids)
h = b'WDC5' + struct.pack('<I', 5) + bytes(128)
h += struct.pack('<IIIIIIIII', 3, 5, record_size, len(strings), 0x12345678, 0xABCDEF01, 0, 530, 0)
h += struct.pack('<HH', 0x04, 0)
h += struct.pack('<IIIIIII', 5, 0, 0, 5 * 24, len(common), len(pallet), 1)
assert len(h) == hdr_len
h += struct.pack('<QIIIIIIII', 0, sec_off, 3, len(strings), 0, len(idlist), 0, 0, 0)
h += b''.join(struct.pack('<hH', 32, f[0] // 8) for f in fields)
h += b''.join(struct.pack('<HHIIIII', *f) for f in fields)
h += pallet + common
assert len(h) == sec_off
open(os.path.join(out, 'Map.db2'), 'wb').write(h + bytes(recs) + bytes(strings) + idlist)

# ---- Map2.db2: same table in 2 sections; section 0 is ENCRYPTED (key hash != 0, 2 records + its own string block), section 1 readable.
# String offsets are relative to the virtual layout: all records of all sections, then every section's string block.
enc_recs = bytes(2 * record_size); enc_str = b'\0SECRET-STRING-IN-THE-ENCRYPTED-SECTION\0'; enc_ids = struct.pack('<II', 7, 8)
total_rs = 5 * record_size
hdr2 = hdr_len + 40
sec0 = hdr2 + 40 + 5 * 4 + 5 * 24 + len(pallet) + len(common)
sec1 = sec0 + len(enc_recs) + len(enc_str) + len(enc_ids)
recs2 = bytearray(record_size * 3); strings2 = bytearray(b'\0')
for i, (d, n) in enumerate(names):
    for f, s_ in ((0, d), (1, n)):
        pos = len(strings2); strings2 += s_.encode() + b'\0'
        vpos = (2 + i) * record_size + fields[f][0] // 8
        struct.pack_into('<I', recs2, i * record_size + fields[f][0] // 8, total_rs - vpos + len(enc_str) + pos)
    recs2[i * record_size + 8] = (i + 1) | ((2 - i) << 3)
h2 = b'WDC5' + struct.pack('<I', 5) + bytes(128)
h2 += struct.pack('<IIIIIIIII', 5, 5, record_size, len(enc_str) + len(strings2), 0x12345678, 0xABCDEF01, 0, 530, 0)
h2 += struct.pack('<HH', 0x04, 0)
h2 += struct.pack('<IIIIIII', 5, 0, 0, 5 * 24, len(common), len(pallet), 2)
h2 += struct.pack('<QIIIIIIII', 0xDEADBEEFCAFEF00D, sec0, 2, len(enc_str), 0, len(enc_ids), 0, 0, 0)
h2 += struct.pack('<QIIIIIIII', 0, sec1, 3, len(strings2), 0, len(idlist), 0, 0, 0)
h2 += b''.join(struct.pack('<hH', 32, f[0] // 8) for f in fields)
h2 += b''.join(struct.pack('<HHIIIII', *f) for f in fields)
h2 += pallet + common
assert len(h2) == sec0
open(os.path.join(out, 'Map2.db2'), 'wb').write(h2 + enc_recs + enc_str + enc_ids + bytes(recs2) + bytes(strings2) + idlist)

# ---- WDT: tile 32_48 and 33_48 with FileDataIDs
maid = bytearray(64 * 64 * 32)
main = bytearray(64 * 64 * 8)
for (x, y), base in (((32, 48), 1000), ((33, 48), 1100)):
    i = y * 64 + x
    struct.pack_into('<8I', maid, i * 32, *[base + k + 1 for k in range(8)])
    struct.pack_into('<I', main, i * 8, 1)
open(os.path.join(out, 'Test.wdt'), 'wb').write(chunk('MVER', struct.pack('<I', 18)) + chunk('MPHD', struct.pack('<I', 0x200) + bytes(28)) + chunk('MAIN', bytes(main)) + chunk('MAID', bytes(maid)))
# ---- ADT obj0: 2 doodads by FileDataID, 1 building by FileDataID
mddf = b''.join(struct.pack('<II6fHH', 189000 + k, k, 0, 0, 0, 0, 0, 0, 1024, 0x40) for k in range(2))
modf = struct.pack('<II6f6fHHHH', 107000, 9, *([0.0] * 12), 0x8, 0, 0, 1024)
open(os.path.join(out, 'Test_32_48_obj0.adt'), 'wb').write(chunk('MVER', struct.pack('<I', 18)) + chunk('MDDF', mddf) + chunk('MODF', modf))
# ---- M2: MD21 with an MD20 header (name 'Wolf', 1000 vertices ...), SFID, TXID, AFID
md20 = bytearray(0x130)
md20[0:4] = b'MD20'
struct.pack_into('<I', md20, 4, 264)
struct.pack_into('<II', md20, 8, 5, 0x120)
md20[0x120:0x125] = b'Wolf\0'
struct.pack_into('<I', md20, 28, 12)     # sequences
struct.pack_into('<I', md20, 44, 40)     # bones
struct.pack_into('<I', md20, 60, 1000)   # vertices
struct.pack_into('<I', md20, 68, 1)      # views
struct.pack_into('<I', md20, 80, 2)      # textures
m2 = chunkf('MD21', bytes(md20)) + chunkf('SFID', struct.pack('<I', 2001)) + chunkf('TXID', struct.pack('<II', 3001, 3002)) + chunkf('AFID', struct.pack('<HHI', 0, 0, 0) + struct.pack('<HHI', 4, 0, 4001))
open(os.path.join(out, 'Wolf.m2'), 'wb').write(m2)
print('written to', out)
