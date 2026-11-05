#!/bin/bash
# Builds the WoW Forever probes: w3dfprobe (login probe) and w3dfdata (CASC / DB2 / map data probe). Linux: build.sh linux <outdir>; Windows: build.sh win <outdir> (llvm-mingw in PATH or MINGW=prefix).
set -e
D=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-linux}; OUT=${2:-$D/build-$MODE}; mkdir -p "$OUT/obj"
if [ "$MODE" = win ]; then CC=${MINGW:-x86_64-w64-mingw32-}clang; CXX=${MINGW:-x86_64-w64-mingw32-}clang++; EXE=.exe; LIBS="-lws2_32 -lbcrypt -lwininet -static"; else CC=${CC:-gcc}; CXX=${CXX:-g++}; EXE=; LIBS="${SANLIBS:-}"; fi
CFL="-O2 -I$D/extern/mbedtls/include -I$D/extern/miniz ${EXTRA_CFLAGS:-}"
objs=()
for f in "$D"/extern/mbedtls/library/*.c "$D"/extern/miniz/miniz.c "$D"/extern/miniz/miniz_tdef.c "$D"/extern/miniz/miniz_tinfl.c; do
  o="$OUT/obj/$(basename "$f" .c).o"
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then $CC $CFL -w -c "$f" -o "$o" & fi
  objs+=("$o")
  while [ "$(jobs -r | wc -l)" -ge 16 ]; do sleep 0.1; done
done
wait
for f in fbase fnet fbnet fworld; do $CXX -std=c++17 $CFL -Wall -Wextra -c "$D/$f.cpp" -o "$OUT/obj/$f.o" & done
wait
$CXX -std=c++17 $CFL -Wall -Wextra "$D/w3dfprobe.cpp" "$OUT"/obj/fbase.o "$OUT"/obj/fnet.o "$OUT"/obj/fbnet.o "$OUT"/obj/fworld.o "${objs[@]}" $LIBS -o "$OUT/w3dfprobe$EXE"
# CascLib (MIT) with its own zlib
C=$D/extern/casclib/src
cobjs=()
for f in $C/common/*.cpp $C/hashes/*.cpp $C/overwatch/apm.cpp $C/overwatch/cmf.cpp $C/overwatch/aes.cpp $C/Casc*.cpp $C/jenkins/lookup3.c $C/zlib/adler32.c $C/zlib/crc32.c $C/zlib/inffast.c $C/zlib/inflate.c $C/zlib/inftrees.c $C/zlib/zutil.c; do
  o="$OUT/obj/casc_$(basename "$f").o"
  case "$f" in *.c) cc="$CC";; *) cc="$CXX -std=c++17";; esac
  if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then $cc -O2 -w -DCASCLIB_NO_AUTO_LINK_LIBRARY -DCASCLIB_NODEBUG -c "$f" -o "$o" & fi
  cobjs+=("$o")
  while [ "$(jobs -r | wc -l)" -ge 16 ]; do sleep 0.1; done
done
wait
$CXX -std=c++17 -O2 -Wall -Wextra -DCASCLIB_NO_AUTO_LINK_LIBRARY -I$C -c "$D/fdata.cpp" -o "$OUT/obj/fdata.o"
$CXX -std=c++17 -O2 -Wall -Wextra -DCASCLIB_NO_AUTO_LINK_LIBRARY -I$C "$D/w3dfdata.cpp" "$OUT/obj/fdata.o" "${cobjs[@]}" $LIBS -o "$OUT/w3dfdata$EXE"
echo "built $OUT/w3dfprobe$EXE $OUT/w3dfdata$EXE"
