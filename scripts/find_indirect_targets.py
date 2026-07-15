#!/usr/bin/env python3
"""Find function-pointer targets in ROM data that are missing from the syms TOML.

Scans the ROM for 4-byte-aligned big-endian words that look like VRAM code
pointers, validates each candidate as a plausible function start (the word 8
bytes before it in the same section must be a `jr $ra`, i.e. the previous
function's return + delay slot, or the candidate must be a section start),
and reports candidates that fall strictly inside an existing symbol (i.e.
merged functions that need splitting) or in gaps.

Usage: python scripts/find_indirect_targets.py [--apply]
  --apply  rewrite the syms TOML with the necessary splits
"""

import re
import struct
import sys

ROM_PATH = "baserom.us.rev1.z64"
SYMS_PATH = "recomp/waverace64.us.rev1.syms.toml"

# (rom_start, vram_start, size) of the two statically mapped code sections.
SECTIONS = [
    (0x00001000, 0x80046800, 0xA85D0),   # .main
    (0x000A95D0, 0x801DAFA0, 0x4CAC0),   # .codeseg
]

JR_RA = 0x03E00008


def split_single(target_vram):
    """Split the function containing target_vram at that address (with jr-ra
    validation). Used by the crash-driven bring-up loop:
    python scripts/find_indirect_targets.py --split 0x8009AA24
    """
    rom = open(ROM_PATH, "rb").read()
    text = open(SYMS_PATH, "r", encoding="utf-8").read()
    pattern = re.compile(
        r'\[\[section\.functions\]\]\s*\nname = "([^"]+)"\s*\nvram = (0x[0-9A-Fa-f]+)\s*\nsize = (0x[0-9A-Fa-f]+)'
    )
    funcs = sorted((int(m.group(2), 16), int(m.group(3), 16), m.group(1)) for m in pattern.finditer(text))

    rom_off = None
    for rom_start, vram_start, size in SECTIONS:
        if vram_start <= target_vram < vram_start + size:
            rom_off = rom_start + (target_vram - vram_start)
    if rom_off is None:
        print(f"REFUSED: 0x{target_vram:08X} not in a static code section")
        return 2
    prev = struct.unpack_from(">I", rom, rom_off - 8)[0]
    if prev != JR_RA:
        print(f"REFUSED: word at 0x{target_vram - 8:08X} is 0x{prev:08X}, not jr $ra — needs manual analysis")
        return 2

    container = None
    for fv, fs, fn in funcs:
        if fv < target_vram < fv + fs:
            container = (fv, fs, fn)
    if container is None:
        if any(fv == target_vram for fv, _, _ in funcs):
            print(f"already a function start: 0x{target_vram:08X}")
            return 1
        print(f"REFUSED: 0x{target_vram:08X} not inside any known function")
        return 2

    cv, cs, cn = container
    old = re.compile(
        r'\[\[section\.functions\]\]\s*\nname = "' + re.escape(cn) +
        r'"\s*\nvram = 0x[0-9A-Fa-f]+\s*\nsize = 0x[0-9A-Fa-f]+'
    )
    replacement = (
        f'[[section.functions]]\nname = "{cn}"\nvram = 0x{cv:08X}\nsize = 0x{target_vram - cv:X}\n\n'
        f'[[section.functions]]\nname = "func_{target_vram:08X}"\nvram = 0x{target_vram:08X}\nsize = 0x{cv + cs - target_vram:X}'
    )
    text, n = old.subn(replacement, text, count=1)
    if n != 1:
        print(f"ERROR: could not rewrite {cn}")
        return 2
    open(SYMS_PATH, "w", encoding="utf-8", newline="\n").write(text)
    print(f"split {cn} at 0x{target_vram:08X}")
    return 0


def main():
    if "--split" in sys.argv:
        target = int(sys.argv[sys.argv.index("--split") + 1], 16)
        sys.exit(split_single(target))

    apply_changes = "--apply" in sys.argv

    rom = open(ROM_PATH, "rb").read()

    # Parse existing function symbols (name, vram, size) from the TOML.
    text = open(SYMS_PATH, "r", encoding="utf-8").read()
    funcs = []  # (vram, size, name)
    pattern = re.compile(
        r'\[\[section\.functions\]\]\s*\nname = "([^"]+)"\s*\nvram = (0x[0-9A-Fa-f]+)\s*\nsize = (0x[0-9A-Fa-f]+)'
    )
    for m in pattern.finditer(text):
        funcs.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
    funcs.sort()
    func_starts = {f[0] for f in funcs}
    print(f"parsed {len(funcs)} functions from syms")

    def vram_to_rom(vram):
        for rom_start, vram_start, size in SECTIONS:
            if vram_start <= vram < vram_start + size:
                return rom_start + (vram - vram_start)
        return None

    def word_at_rom(off):
        return struct.unpack_from(">I", rom, off)[0]

    def plausible_function_start(vram):
        """The 8 bytes before must end a previous function (jr $ra + delay)."""
        rom_off = vram_to_rom(vram)
        if rom_off is None or rom_off < 8:
            return False
        for rom_start, vram_start, _ in SECTIONS:
            if vram == vram_start:
                return True
        prev_instr = word_at_rom(rom_off - 8)
        return prev_instr == JR_RA

    def containing_function(vram):
        lo, hi = 0, len(funcs) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            fv, fs, fn = funcs[mid]
            if fv <= vram < fv + fs:
                return funcs[mid]
            if vram < fv:
                hi = mid - 1
            else:
                lo = mid + 1
        return None

    def is_code_pointer(w):
        for _, vram_start, size in SECTIONS:
            if vram_start <= w < vram_start + size and (w & 3) == 0:
                return True
        return False

    # Scan the whole ROM for words that look like code pointers, but skip
    # jump-table entries: a switch jump table is a run of stride-4 consecutive
    # pointers whose targets all land in the SAME function. Splitting those
    # breaks the recompiler's jump-table sizing. Real dispatch tables in this
    # game are stride-8 records ({param, handler}), so their pointer words have
    # non-pointer neighbors.
    candidates = set()
    for off in range(0, len(rom) - 3, 4):
        w = struct.unpack_from(">I", rom, off)[0]
        if not is_code_pointer(w):
            continue
        container = containing_function(w)

        def neighbor_same_container(noff):
            if noff < 0 or noff + 4 > len(rom):
                return False
            nw = struct.unpack_from(">I", rom, noff)[0]
            if not is_code_pointer(nw):
                return False
            ncontainer = containing_function(nw)
            return container is not None and ncontainer == container

        # Jump-table entry: at least one stride-4 neighbor targets the same function.
        if neighbor_same_container(off - 4) or neighbor_same_container(off + 4):
            continue
        candidates.add(w)
    print(f"pointer-like words found (jump tables filtered): {len(candidates)}")

    # Classify: which candidates are not existing function starts but fall
    # inside an existing function (merged) and pass the jr-ra check?
    splits = []  # (container_vram, container_size, container_name, split_vram)
    for w in sorted(candidates):
        if w in func_starts:
            continue
        if not plausible_function_start(w):
            continue
        # find containing function
        lo, hi = 0, len(funcs) - 1
        container = None
        while lo <= hi:
            mid = (lo + hi) // 2
            fv, fs, fn = funcs[mid]
            if fv <= w < fv + fs:
                container = funcs[mid]
                break
            if w < fv:
                hi = mid - 1
            else:
                lo = mid + 1
        if container and w > container[0]:
            splits.append((container[0], container[1], container[2], w))

    print(f"validated split points inside existing functions: {len(splits)}")
    for cv, cs, cn, sv in splits:
        print(f"  split {cn} (0x{cv:08X} size 0x{cs:X}) at 0x{sv:08X}")

    if not apply_changes:
        print("\n(dry run — pass --apply to rewrite the syms TOML)")
        return

    # Apply splits: group by container, sort split points, rewrite entries.
    from collections import defaultdict
    by_container = defaultdict(list)
    for cv, cs, cn, sv in splits:
        by_container[(cv, cs, cn)].append(sv)

    for (cv, cs, cn), points in by_container.items():
        points = sorted(set(points))
        bounds = [cv] + points + [cv + cs]
        entries = []
        for i in range(len(bounds) - 1):
            v, sz = bounds[i], bounds[i + 1] - bounds[i]
            name = cn if v == cv else f"func_{v:08X}"
            entries.append(
                f'[[section.functions]]\nname = "{name}"\nvram = 0x{v:08X}\nsize = 0x{sz:X}'
            )
        old = re.compile(
            r'\[\[section\.functions\]\]\s*\nname = "' + re.escape(cn) +
            r'"\s*\nvram = 0x[0-9A-Fa-f]+\s*\nsize = 0x[0-9A-Fa-f]+'
        )
        replacement = "\n\n".join(entries)
        text, n = old.subn(replacement.replace("\\", "\\\\"), text, count=1)
        if n != 1:
            print(f"WARNING: failed to rewrite {cn}")

    open(SYMS_PATH, "w", encoding="utf-8", newline="\n").write(text)
    print(f"\nrewrote {SYMS_PATH} with {len(splits)} splits across {len(by_container)} functions")


if __name__ == "__main__":
    main()
