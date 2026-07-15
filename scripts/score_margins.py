#!/usr/bin/env python3
"""Score how much real content the margins of a framebuffer dump contain.

The game culls objects/detailed water to the original view rect
(8,20)-(310,218). Culled margins show only the flat background ocean (low
luminance variance); if culling is widened, waves/objects spill into the
margins and variance jumps. Prints a single float score.
"""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "fb_dump.bin"
raw = open(path, "rb").read()
px = struct.unpack(f"<{320*240}H", raw[: 320 * 240 * 2])

def lum(p):
    return ((p >> 11) & 0x1F) * 3 + ((p >> 6) & 0x1F) * 6 + ((p >> 1) & 0x1F)

def stats(pixels):
    n = len(pixels)
    mean = sum(pixels) / n
    var = sum((v - mean) ** 2 for v in pixels) / n
    return mean, var ** 0.5

margin = []
interior_edge = []  # interior band adjacent to the margins, for comparison
for y in range(240):
    for x in range(320):
        v = lum(px[y * 320 + x])
        in_rect = 8 <= x < 310 and 20 <= y < 218
        if not in_rect:
            margin.append(v)
        elif x < 24 or x >= 294 or y < 36 or y >= 202:
            interior_edge.append(v)

m_mean, m_std = stats(margin)
i_mean, i_std = stats(interior_edge)
# Score: margin texture (std) relative to interior edge texture, plus the
# mean-color seam penalty inverted (smaller seam = higher score).
seam = abs(m_mean - i_mean)
score = m_std / (i_std + 1e-6) * 100 - seam
print(f"{score:.2f}  (margin_std={m_std:.2f} interior_std={i_std:.2f} seam={seam:.2f})")
