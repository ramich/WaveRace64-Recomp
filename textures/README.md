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

## Caveats

- Replacements draw from a VRAM pool alongside the framebuffers. HD textures
  *and* high internal resolution (Graphics → Resolution 3x/4x + downsampling)
  both consume VRAM; combining them can exhaust it (`DXGI_ERROR_DEVICE_REMOVED`).
- One replacement applies everywhere its hash appears; textures that animate or
  share memory layout may need per-frame care.

This folder is not committed with any textures — it's a drop point + docs.
