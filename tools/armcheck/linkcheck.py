#!/usr/bin/env python3
"""6.35b: a link check for the ARM objects armcheck.sh leaves in $TMPDIR (/tmp/arm_*.o): every C++ symbol one of them needs
must be defined by one of them (libctru / citro3d / the C++ library aside). Catches what a compile-only check cannot: a
function or variable declared in a header but defined inside an anonymous namespace (7r: __ctru_heap_size; 8a: perf::vramWatch).
  bash tools/armcheck/armcheck.sh proto3ds/source/*.cpp net/*.cpp && python3 tools/armcheck/linkcheck.py"""
import glob, os, re, subprocess, sys
objs = glob.glob(os.path.join(os.environ.get("TMPDIR", "/tmp"), "arm_*.o"))
defined, undef = set(), {}
for o in objs:
    for l in subprocess.run(["llvm-nm", o], capture_output=True, text=True).stdout.splitlines():
        p = l.split()
        if len(p) == 2 and p[0] == "U":
            undef.setdefault(p[1], []).append(os.path.basename(o))
        elif len(p) == 3:
            defined.add(p[2])
lib = re.compile(r"_Z(N?K?St|St|nw|na|dl|da|N9__gnu|NK9__gnu|N10__cxxabi|TV|TI|TS)")
missing = [s for s in undef if s not in defined and s.startswith("_Z") and not lib.match(s)]
names = subprocess.run(["llvm-cxxfilt"] + missing, capture_output=True, text=True).stdout.splitlines() if missing else []
for s, d in zip(missing, names):
    print("UNDEFINED:", d, "<-", ", ".join(undef[s][:3]))
print(f"link check: {len(objs)} objects, {len(missing)} C++ symbols referenced but defined nowhere -> {'FAIL' if missing else 'PASS'}")
sys.exit(1 if missing else 0)
