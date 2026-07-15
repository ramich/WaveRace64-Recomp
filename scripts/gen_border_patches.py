#!/usr/bin/env python3
"""Find every code site that builds the border G_SETSCISSOR (8,20)-(310,218)
(w0 0xED020050, w1 0x004D8368) in the recompiled output, and emit
[[patches.instruction]] TOML entries rewriting it to (0,0)-(320,240)
(w0 0xED000000, w1 0x005003C0).

The recompiled C carries the original disassembly as comments, giving us the
vram, mnemonic, and registers of every instruction — enough to re-encode the
patched instruction words exactly (lui/ori encodings are deterministic).
"""

import glob
import re

REGS = {
    "$zero": 0, "$at": 1, "$v0": 2, "$v1": 3, "$a0": 4, "$a1": 5, "$a2": 6, "$a3": 7,
    "$t0": 8, "$t1": 9, "$t2": 10, "$t3": 11, "$t4": 12, "$t5": 13, "$t6": 14, "$t7": 15,
    "$s0": 16, "$s1": 17, "$s2": 18, "$s3": 19, "$s4": 20, "$s5": 21, "$s6": 22, "$s7": 23,
    "$t8": 24, "$t9": 25, "$k0": 26, "$k1": 27, "$gp": 28, "$sp": 29, "$fp": 30, "$ra": 31,
}

def lui(rt, imm):
    return 0x3C000000 | (REGS[rt] << 16) | (imm & 0xFFFF)

def ori(rt, rs, imm):
    return 0x34000000 | (REGS[rs] << 21) | (REGS[rt] << 16) | (imm & 0xFFFF)

COMMENT = re.compile(r"// (0x[0-9A-Fa-f]{8}): (\w+)\s+(.*)")
FUNC = re.compile(r"RECOMP_FUNC void (\w+)\(")

patches = []  # (func, vram, newword, comment)

for path in sorted(glob.glob("RecompiledFuncs/funcs_*.c")):
    cur_func = None
    # Collect (vram, mnemonic, args, func) tuples in order.
    instrs = []
    for line in open(path, encoding="utf-8", errors="replace"):
        fm = FUNC.search(line)
        if fm:
            cur_func = fm.group(1)
            continue
        cm = COMMENT.search(line)
        if cm and cur_func:
            instrs.append((int(cm.group(1), 16), cm.group(2), cm.group(3).strip(), cur_func))

    # Find each 'ori X, X, 0x8368' and its companions within +/-12 instructions.
    for i, (vram, mnem, args, func) in enumerate(instrs):
        if mnem != "ori" or not args.endswith("0x8368"):
            continue
        window = instrs[max(0, i - 12): i + 12]
        w0_lui = next((t for t in window if t[1] == "lui" and t[2].endswith("0xED02")), None)
        w1_lui = next((t for t in window if t[1] == "lui" and t[2].endswith("0x4D")), None)
        if not w0_lui or not w1_lui:
            continue
        # w0's ori pairs with w0_lui's register.
        w0_reg = w0_lui[2].split(",")[0].strip()
        w0_ori = next((t for t in window if t[1] == "ori" and t[2].startswith(f"{w0_reg}, {w0_reg},") and t[2].endswith("0x50")), None)
        if not w0_ori:
            continue
        w1_reg = w1_lui[2].split(",")[0].strip()

        # Skip already-patched or duplicate discoveries.
        key = (func, w0_lui[0])
        if any(p[0] == func and p[1] == w0_lui[0] for p in patches):
            continue

        patches.append((func, w0_lui[0], lui(w0_reg, 0xED00), "lui %s, 0xED00" % w0_reg))
        patches.append((func, w0_ori[0], ori(w0_reg, w0_reg, 0x0000), "ori %s, %s, 0x0" % (w0_reg, w0_reg)))
        patches.append((func, w1_lui[0], lui(w1_reg, 0x0050), "lui %s, 0x50" % w1_reg))
        patches.append((func, vram, ori(w1_reg, w1_reg, 0x03C0), "ori %s, %s, 0x3C0" % (w1_reg, w1_reg)))

for func, vram, word, comment in patches:
    print(f'''[[patches.instruction]]
func = "{func}"
vram = 0x{vram:08X}
value = 0x{word:08X} # {comment}
''')
print(f"# total: {len(patches)} instruction patches across {len(set(p[0] for p in patches))} functions")
