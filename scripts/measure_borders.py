#!/usr/bin/env python3
"""Measure black borders in a 320x240 RGBA5551 framebuffer dump (little-endian u16s)."""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "fb_dump.bin"
raw = open(path, "rb").read()
px = struct.unpack(f"<{320*240}H", raw[: 320 * 240 * 2])

def is_black(p):
    r = (p >> 11) & 0x1F
    g = (p >> 6) & 0x1F
    b = (p >> 1) & 0x1F
    return r + g + b < 3

def row_black(y):
    return all(is_black(px[y * 320 + x]) for x in range(320))

def col_black(x):
    return all(is_black(px[y * 320 + x]) for y in range(240))

top = next((y for y in range(240) if not row_black(y)), 240)
bottom = next((y for y in range(240) if not row_black(239 - y)), 240)
left = next((x for x in range(320) if not col_black(x)), 320)
right = next((x for x in range(320) if not col_black(319 - x)), 320)
print(f"{path}: borders top={top} bottom={bottom} left={left} right={right}")
