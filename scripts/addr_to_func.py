#!/usr/bin/env python3
"""Map VRAM addresses to their containing functions using the syms TOML.
Usage: python scripts/addr_to_func.py 0x8008BE96 0x8008C016 ..."""
import re
import sys

text = open("recomp/waverace64.us.rev1.syms.toml").read()
funcs = sorted(
    (int(m.group(2), 16), int(m.group(3), 16), m.group(1))
    for m in re.finditer(
        r'\[\[section\.functions\]\]\s*\nname = "([^"]+)"\s*\nvram = (0x[0-9A-Fa-f]+)\s*\nsize = (0x[0-9A-Fa-f]+)',
        text,
    )
)
for arg in sys.argv[1:]:
    addr = int(arg, 16)
    hit = None
    for v, s, n in funcs:
        if v <= addr < v + s:
            hit = (v, s, n)
            break
    if hit:
        print(f"0x{addr:08X} -> {hit[2]} (0x{hit[0]:08X} size 0x{hit[1]:X}, offset +0x{addr - hit[0]:X})")
    else:
        print(f"0x{addr:08X} -> (no containing function)")
