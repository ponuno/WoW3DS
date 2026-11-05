#!/bin/bash
# 6.63: quest areas. A small sample in Questie's data format -> tools/questdb/questie_import.py -> questdb.bin -> questdb_test
# (questdb::Db and questwatch::Watch). With QUESTIE_DATA=<QuestieDB/data/Classic> the real data goes through the importer too.
#   run_questdb.sh
set -e -o pipefail
T=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/tmp/w3d_questdb}; rm -rf "$OUT"; mkdir -p "$OUT/lua"
python3 - "$OUT/lua" <<'PY'
import sys
d = sys.argv[1]
def wrap(name, rows):
    return "-- sample\nlocal QuestieDB = QuestieLoader:ImportModule(\"QuestieDB\");\nQuestieDB.%s = [[return {\n%s\n}]]\n" % (name, "\n".join(rows))
def pts(zone, cx, cy, n, spread=0.4):
    return "{[%d]={%s}}" % (zone, ",".join("{%.2f,%.2f}" % (cx + spread * ((i % 4) - 1.5), cy + spread * ((i // 4) - 1)) for i in range(n)))
npc = [
 "[823] = {'Deputy Willem',100,100,5,5,0,%s,nil,12,{6,7},{6,7},12,'AH',nil,0}," % pts(12, 48.17, 42.94, 1, 0.0),
 "[103] = {'Garrick Padfoot',100,100,5,5,0,{[12]={{57.4,48.0},{57.6,48.2},{57.5,48.4},{20.0,70.0},{20.1,70.1}}},nil,12,nil,nil,12,nil,nil,0},",
 "[6] = {'Kobold Vermin',42,55,1,2,0,%s,nil,12,nil,nil,21,nil,nil,0}," % pts(12, 48.9, 36.4, 12),
 "[777] = {'Far Away',42,55,1,2,0,{[12]={{150.0,50.0},{-3.0,20.0}}},nil,12,nil,nil,21,nil,nil,0},",
 "[800] = {'Credit A',42,55,1,2,0,%s,nil,12,nil,nil,21,nil,nil,0}," % pts(12, 48.9, 36.4, 8),
]
obj = [
 "[31] = {\"Old Lion Statue\",{8},nil,{[44]={{84.49,46.83}}},44},",
]
item = [
 "[182] = {'Garrick\\'s Head',{103},nil,nil,nil,nil,nil,nil,1,1,0,12,0},",
]
q = [
 "[6] = {\"Bounty on Garrick Padfoot\",{{823}},{{823}},2,5,77,nil,{\"Kill Garrick\"},nil,{nil,nil,{{182}}},nil,nil,nil,nil,nil,nil,12},",
 "[7] = {\"Kobold Camp Cleanup\",{{823}},{{823}},2,5,77,nil,{\"Kill\"},nil,{{{6,\"Kobold slain\"}}},nil,nil,nil,nil,nil,nil,12},",
 "[8] = {\"Statue\",nil,nil,2,5,77,nil,{\"Use\"},{\"Find the place\",{[44]={{30.0,30.0},{31.0,30.5}}}},{nil,{{31}}},nil,nil,nil,nil,nil,nil,44},",
 "[9] = {\"Far Away Handin\",nil,{{777}},2,5,77,nil,{\"Go\"},nil,nil,nil,nil,nil,nil,nil,nil,12},",
 "[10] = {\"Kill Credit\",nil,nil,2,5,77,nil,{\"Kill\"},nil,{nil,nil,nil,nil,{{{6,800},6,\"Kobolds\"}}},nil,nil,nil,nil,nil,nil,12},",
 "[11] = {\"Nothing known\",{{999}},{{999}},2,5,77,nil,{\"x\"},nil,nil},",
]
open(d + "/classicNpcDB.lua", "w").write(wrap("npcData", npc))
open(d + "/classicObjectDB.lua", "w").write(wrap("objectData", obj))
open(d + "/classicItemDB.lua", "w").write(wrap("itemData", item))
open(d + "/classicQuestDB.lua", "w").write(wrap("questData", q))
PY
python3 "$T/../../tools/questdb/questie_import.py" "$OUT/lua" "$OUT/questdb.bin"
g++ -std=c++20 -g -O1 -Wall -fsanitize=address,undefined "$T/questdb_test.cpp" "$T/../source/questdb.cpp" "$T/../source/questwatch.cpp" -o "$OUT/questdb_test"
python3 - "$OUT/v2.bin" <<'PY'
import struct, sys
def pt(m, x, y): return struct.pack('<Hhh', m, x, y)
def blob(m, x, y, r): return struct.pack('<HhhH', m, x, y, r)
r100 = struct.pack('<BBBB', 1, 1, 1, 0) + pt(0, -8913, -133) + pt(1, 10312, -2000) + struct.pack('<BBHI', 0, 2, 0, 299) + blob(0, -8949, -132, 20) + blob(0, -9018, -164, 40)
r105 = struct.pack('<BBBB', 0, 0, 1, 0) + struct.pack('<BBHI', 2, 1, 0, 777) + blob(1, 5, 5, 6)
off = 16 + 16
open(sys.argv[1], 'wb').write(b'WQDB' + struct.pack('<IIIIIII', 2, 2, 16, 100, off, 105, off + len(r100)) + r100 + r105)
PY
"$OUT/questdb_test" "$OUT/questdb.bin" "$OUT/v2.bin"
if [ -n "$QUESTIE_DATA" ]; then python3 "$T/../../tools/questdb/questie_import.py" "$QUESTIE_DATA" "$OUT/real.bin"; fi
