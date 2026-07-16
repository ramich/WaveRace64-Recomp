#!/usr/bin/env python3
"""Emit [[patches.instruction]] entries widening every inline 45.0f camera
constant (lui $reg, 0x4234) to 47.75f (0x423F), preserving each site's target
register. Prints the TOML block to stdout."""
import glob
import re

REGS = {
    "$zero": 0, "$at": 1, "$v0": 2, "$v1": 3, "$a0": 4, "$a1": 5, "$a2": 6, "$a3": 7,
    "$t0": 8, "$t1": 9, "$t2": 10, "$t3": 11, "$t4": 12, "$t5": 13, "$t6": 14, "$t7": 15,
    "$s0": 16, "$s1": 17, "$s2": 18, "$s3": 19, "$s4": 20, "$s5": 21, "$s6": 22, "$s7": 23,
    "$t8": 24, "$t9": 25, "$k0": 26, "$k1": 27, "$gp": 28, "$sp": 29, "$fp": 30, "$ra": 31,
}
FUNC = re.compile(r"RECOMP_FUNC void (\w+)\(")
SITE = re.compile(r"// (0x[0-9A-Fa-f]{8}): lui\s+(\$\w+), 0x4234$")

entries = {}
for path in sorted(glob.glob("RecompiledFuncs/funcs_*.c")):
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        fm = FUNC.search(line)
        if fm:
            cur = fm.group(1)
            continue
        sm = SITE.search(line.rstrip())
        if sm and cur:
            vram, reg = int(sm.group(1), 16), sm.group(2)
            word = 0x3C000000 | (REGS[reg] << 16) | 0x423F
            entries[(cur, vram)] = (word, reg)

for (func, vram), (word, reg) in sorted(entries.items(), key=lambda e: e[0][1]):
    print(f'[[patches.instruction]]\nfunc = "{func}"\nvram = 0x{vram:08X}\nvalue = 0x{word:08X} # lui {reg}, 0x423F (47.75f, was 45.0f)\n')
print(f"# total: {len(entries)} sites")
