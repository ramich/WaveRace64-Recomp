# HD texture packs (RT64 replacement textures)

Wave Race 64 Recomp uses RT64's built-in texture-replacement system. You can
replace any of the game's textures (HUD, water, etc.) with higher-resolution
images of your own — no engine changes needed.

## Enabling a pack

Point the game at a pack directory and it loads at launch (and is enabled — you
can toggle it live with **F4**):

```powershell
$env:WR64_TEXPACK = "textures"   # this folder; or an absolute path
.\WaveRace64Recomp.exe
```

`WR64_TEXPACK=1` is shorthand for `.\textures`. A pack directory must contain an
`rt64.json` database plus the replacement image files it references; loading a
folder without `rt64.json` is reported and skipped.

## Building a pack (the workflow)

1. **Dump the game's textures** so you get their hashes:
   ```powershell
   $env:WR64_TEXDUMP = "textures_dump"   # or =1 for .\textures_dump
   .\WaveRace64Recomp.exe
   ```
   Play through the screens whose textures you want (e.g. sit on the HUD during a
   race). RT64 writes each texture it loads into that folder, named by its
   64-bit hash (`<hash>.vN.tmem`, `<hash>.rice.rdram`, `<hash>.rice.json`, …).
   The 16-hex prefix is the **hash** — the key a replacement is matched on.

2. **Convert the dump into a viewable/editable pack** using RT64's texture-pack
   tooling (the dump is raw N64 texture memory, not PNGs). That step produces an
   `rt64.json` database and per-texture images you can open.

3. **Identify the HUD textures** among the images and replace them with your
   higher-resolution versions (any resolution; RT64 maps them onto the same
   tile). Keep each replacement keyed to the same hash in `rt64.json`.

4. **Drop the finished pack here** (or anywhere) and run with `WR64_TEXPACK`
   pointing at it.

## Using a community Rice-format pack

Many N64 hi-res packs use Rice/GLideN64 naming (`<rom>#<crc>#<fmt>#<siz>_all.png`,
often in subfolders). RT64 matches replacements by its own hash at runtime, so a
Rice pack needs a database mapping each texture's Rice hash → its RT64 hash. Build
one from your dump (RT64 walks subfolders automatically):

```
# 1. Dump the screens/courses the pack covers (WR64_TEXDUMP=1), then:
python scripts/decode_texture_dump.py textures_dump --rice "<pack root folder>"
# 2. Point the Textures tab / WR64_TEXPACK at that pack root folder.
```

This writes only `rt64.json` (a hash index) into the pack folder; the pack images
are referenced in place, not copied. Coverage = whatever is in your dump, so dump
more and re-run to map more. Textures the dump doesn't contain are reported as
"unmatched".

## Caveats

- Replacements draw from a VRAM pool alongside the framebuffers. HD textures
  *and* high internal resolution (Graphics → Resolution 3x/4x + downsampling)
  both consume VRAM; combining them can exhaust it (`DXGI_ERROR_DEVICE_REMOVED`).
- One replacement applies everywhere its hash appears; textures that animate or
  share memory layout may need per-frame care.

This folder is not committed with any textures — it's a drop point + docs.
