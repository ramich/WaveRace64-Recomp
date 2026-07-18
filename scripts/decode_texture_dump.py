"""Decode an RT64 texture dump into viewable PNGs, contact sheets, and a
directly-loadable replacement pack.

Point WR64_TEXDUMP at a folder to produce a dump, then run this on it. Reads the
linear `.rice.rdram` (+ `.rice.palette.rdram` for CI8) using the dimensions and
format from each `.tile.json`. Writes, under <dump>:
  png/<hash>.png     true RGBA (edit these) + png/rt64.json  -> a loadable pack
                     (set WR64_TEXPACK=<dump>/png to see edits in-game; F4 toggles)
  _index_NN.png      labeled contact sheets (hash under each thumbnail)
  _index.txt         hash -> format WxH listing

Usage:  python scripts/decode_texture_dump.py [dump_dir]   (default: ./textures_dump)

Requires Pillow. Original code; implements the public N64 texture formats
(RGBA16/32, CI8, IA8/16, I8) and this runtime's 32-bit-word byte-swap.
"""
import glob, json, os, sys
from PIL import Image, ImageDraw, ImageFont

DUMP = sys.argv[1] if len(sys.argv) > 1 else "textures_dump"
if not os.path.isdir(DUMP):
    sys.exit(f"dump directory not found: {DUMP}\n"
             f"usage: python {os.path.basename(sys.argv[0])} [dump_dir]")
OUT = os.path.join(DUMP, "png")
os.makedirs(OUT, exist_ok=True)

BPP = {(0, 2): 2, (0, 3): 4, (2, 1): 1, (3, 1): 1, (3, 2): 2, (4, 1): 1}
FMTNAME = {(0, 2): "RGBA16", (0, 3): "RGBA32", (2, 1): "CI8",
           (3, 1): "IA8", (3, 2): "IA16", (4, 1): "I8"}

def c5(x):  # 5-bit -> 8-bit
    return (x << 3) | (x >> 2)

def rgba5551(hi, lo):
    v = (hi << 8) | lo
    r = c5((v >> 11) & 0x1F); g = c5((v >> 6) & 0x1F); b = c5((v >> 1) & 0x1F)
    a = 255 if (v & 1) else 0
    return (r, g, b, a)

def wswap(b):
    """Reverse each 4-byte word: this runtime stores RDRAM byte-swapped within
    32-bit words (the ^2/^3 addressing), so the raw dump must be un-swapped to
    true big-endian before decoding."""
    b = bytes(b)
    if len(b) % 4:
        b = b + bytes(4 - (len(b) % 4))
    out = bytearray(len(b))
    for i in range(0, len(b), 4):
        out[i:i+4] = b[i:i+4][::-1]
    return bytes(out)

def decode(base, tile, w, h):
    fmt, siz = tile["fmt"], tile["siz"]
    data = wswap(open(base + ".rice.rdram", "rb").read())
    px = bytearray(w * h * 4)
    key = (fmt, siz)
    if key == (2, 1):  # CI8
        pal = wswap(open(base + ".rice.palette.rdram", "rb").read())
        lut = [rgba5551(pal[i * 2], pal[i * 2 + 1]) for i in range(len(pal) // 2)]
        for i in range(min(w * h, len(data))):
            r, g, b, a = lut[data[i]] if data[i] < len(lut) else (255, 0, 255, 255)
            px[i*4:i*4+4] = bytes((r, g, b, a))
    elif key == (0, 2):  # RGBA16
        for i in range(w * h):
            if i*2+1 >= len(data): break
            px[i*4:i*4+4] = bytes(rgba5551(data[i*2], data[i*2+1]))
    elif key == (0, 3):  # RGBA32
        for i in range(w * h):
            if i*4+3 >= len(data): break
            px[i*4:i*4+4] = data[i*4:i*4+4]
    elif key == (3, 1):  # IA8 (4I+4A)
        for i in range(w * h):
            if i >= len(data): break
            b = data[i]; I = ((b >> 4) & 0xF) * 17; A = (b & 0xF) * 17
            px[i*4:i*4+4] = bytes((I, I, I, A))
    elif key == (3, 2):  # IA16
        for i in range(w * h):
            if i*2+1 >= len(data): break
            I = data[i*2]; A = data[i*2+1]
            px[i*4:i*4+4] = bytes((I, I, I, A))
    elif key == (4, 1):  # I8
        for i in range(w * h):
            if i >= len(data): break
            I = data[i]
            px[i*4:i*4+4] = bytes((I, I, I, 255))
    else:
        return None
    return Image.frombytes("RGBA", (w, h), bytes(px))

def checker(img):
    """Composite over a checkerboard so alpha is visible."""
    w, h = img.size
    bg = Image.new("RGBA", (w, h))
    d = bg.load()
    for y in range(h):
        for x in range(w):
            d[x, y] = (90, 90, 90, 255) if ((x // 4 + y // 4) & 1) else (140, 140, 140, 255)
    return Image.alpha_composite(bg, img)

entries = []
for tj in sorted(glob.glob(os.path.join(DUMP, "*.tile.json"))):
    base = tj[:-len(".tile.json")]
    hashname = os.path.basename(base).split(".")[0]
    t = json.load(open(tj)); tile = t["tile"]; w = t["width"]; h = t["height"]
    key = (tile["fmt"], tile["siz"])
    if key not in BPP or w <= 0 or h <= 0 or w > 1024 or h > 1024:
        continue
    try:
        img = decode(base, tile, w, h)
    except Exception:
        img = None
    if img is None:
        continue
    img.save(os.path.join(OUT, hashname + ".png"))
    entries.append((hashname, FMTNAME[key], w, h, img))

entries.sort(key=lambda e: (e[1], -(e[2] * e[3])))
with open(os.path.join(DUMP, "_index.txt"), "w") as f:
    for name, fmt, w, h, _ in entries:
        f.write(f"{name}  {fmt}  {w}x{h}\n")

# Make the png/ folder a directly-loadable RT64 pack: write rt64.json keyed by
# hash. autoPath=rt64 + files named <hash>.png => RT64 matches them live. Edit
# any png/<hash>.png and point WR64_TEXPACK at this folder to see it in-game.
db = {
    "configuration": {
        "configurationVersion": 3, "autoPath": "rt64",
        "defaultOperation": "stream", "defaultShift": "half", "hashVersion": 5,
    },
    "textures": [{"path": f"{name}.png", "hashes": {"rt64": name, "rice": ""}}
                 for name, _, _, _, _ in entries],
    "operationFilters": [], "shiftFilters": [], "extraFiles": [],
}
with open(os.path.join(OUT, "rt64.json"), "w") as f:
    json.dump(db, f, indent=2)

# Contact sheets
CELL, PAD, COLS = 112, 8, 10
LABEL_H = 14
per = COLS * 8  # 80 per sheet
try:
    font = ImageFont.truetype("consola.ttf", 9)
except Exception:
    font = ImageFont.load_default()
for s in range((len(entries) + per - 1) // per):
    chunk = entries[s*per:(s+1)*per]
    rows = (len(chunk) + COLS - 1) // COLS
    sheet = Image.new("RGBA", (COLS*(CELL+PAD)+PAD, rows*(CELL+LABEL_H+PAD)+PAD),
                      (30, 30, 30, 255))
    dr = ImageDraw.Draw(sheet)
    for i, (name, fmt, w, h, img) in enumerate(chunk):
        cx = PAD + (i % COLS) * (CELL + PAD)
        cy = PAD + (i // COLS) * (CELL + LABEL_H + PAD)
        thumb = checker(img).convert("RGBA")
        sc = min(CELL / w, CELL / h, 8)
        thumb = thumb.resize((max(1, int(w*sc)), max(1, int(h*sc))), Image.NEAREST)
        sheet.alpha_composite(thumb, (cx + (CELL-thumb.width)//2, cy + (CELL-thumb.height)//2))
        dr.rectangle([cx, cy, cx+CELL, cy+CELL], outline=(80, 80, 80, 255))
        dr.text((cx, cy+CELL+2), f"{name[:12]}", fill=(220, 220, 220, 255), font=font)
    sheet.convert("RGB").save(os.path.join(DUMP, f"_index_{s:02d}.png"))

print(f"decoded {len(entries)} textures -> {OUT}")
print(f"contact sheets: {(len(entries)+per-1)//per}  (_index_00.png ...)")
print(f"listing: {os.path.join(DUMP, '_index.txt')}")
