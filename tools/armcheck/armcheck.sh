#!/bin/bash
# ARM11 (mpcore, hard-float) compile check with system clang + libc++18 headers + devkitPro newlib/libctru/citro3d
D=/home/claude/deps; A=$(cd "$(dirname "$0")" && pwd); W=${W:-$(cd "$A/../.." && pwd)}
D=${DEPS:-$D}
RES=$(clang -print-resource-dir)
for f in "$@"; do
clang++ --target=arm-none-eabihf -mcpu=mpcore -mfloat-abi=hard -D__3DS__ -D__DEVKITARM__ -D_GNU_SOURCE -U__INT32_TYPE__ -U__UINT32_TYPE__ "-D__INT32_TYPE__=long int" "-D__UINT32_TYPE__=long unsigned int" -Wno-macro-redefined \
 -nostdinc -nostdinc++ -isystem $A/cfg -isystem $D/llvm/libcxx/include -isystem $D/llvm/libcxxabi/include -isystem $A/stub \
 -isystem $D/newlib/newlib/libc/sys/arm -isystem $D/newlib/newlib/libc/include -isystem $D/newlib/newlib/libc/machine/arm -isystem $RES/include \
 -I$A -I$W/common -I$W/net -I$W/proto3ds/source -I$D/libctru/libctru/include -I$D/citro3d/include \
 -std=gnu++20 -fno-rtti -fno-exceptions -O2 -Wall -Wno-unused-function -c "$f" -o ${TMPDIR:-/tmp}/arm_$(basename $f).o || echo "FAIL $f"
done
