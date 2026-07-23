# WaveRace64-Recomp

A native PC port of **Wave Race 64** (USA Rev 1) using [N64Recomp](https://github.com/N64Recomp/N64Recomp) static recompilation.

> **This fork** ([ramich/WaveRace64-Recomp](https://github.com/ramich/WaveRace64-Recomp), branch `windows-bringup`)
> adds Windows build support and the port's **first working execution**: the game
> boots, renders, responds to keyboard/controller input, and plays audio at the
> correct pitch. See [docs/WINDOWS-BRINGUP.md](docs/WINDOWS-BRINGUP.md) for the full
> bring-up report. Requires the companion
> [ramich/N64ModernRuntime](https://github.com/ramich/N64ModernRuntime)
> `overlay-dma-autoload` branch (wired via the submodule).

---

## AI-Assisted Development

**This fork** — its Windows bring-up, the overlay-loading mechanism, audio/input
wiring, widescreen and per-scene aspect presentation, and the border/culling
reverse-engineering work — was developed primarily through AI-assisted development
(Claude), working iteratively with a human collaborator who supplied hardware
access, visual/audio verification, and judgment calls. This does not describe the
upstream [WACOMalt/WaveRace64-Recomp](https://github.com/WACOMalt/WaveRace64-Recomp)
project, only the changes on this fork's branches. Sessions relied on runtime
instrumentation, automated crash-fix loops, and (later) a scripted input driver for
autonomous UI testing, alongside standard human play-testing and screenshot/log
review. See [docs/RE-NOTES.md](docs/RE-NOTES.md) and
[docs/WINDOWS-BRINGUP.md](docs/WINDOWS-BRINGUP.md) for the technical trail this
process left behind.

---

## Current Status

| Phase | Description | Status |
|-------|-------------|--------|
| **Phase 1** | Environment Setup & Toolchain | **COMPLETE** |
| **Phase 2** | Static Recompilation | **COMPLETE** |
| **Phase 3** | Runtime Integration | **COMPLETE** |
| **Phase 4** | Build & Link (RT64 + Runtime) | **COMPLETE** |
| **Phase 5** | Audio & Input | **COMPLETE** -- input verified by hand, audio via recompiled RSP microcode at 32 kHz |
| **Phase 6** | Game-Specific Fixes | **IN PROGRESS** -- attract mode, menus, and races stable (races exercised extensively by the automated race driver, `scripts/drive_to_race.ps1`). One reproducible crash in a later course is under investigation (see Known Limitations) |
| **Phase 7** | Enhancements | **IN PROGRESS** -- widescreen (RT64 Expand) with a stock-borders mode, per-scene 4:3 menus, camera FOV widened via shipped instruction patches, FPS + target-framerate readout, dev inspector, resolution up to 4x, custom launcher branding (background art + wordmark + app icon), **full-window presentation via Overscan Crop** (1P + 2P split-screen, GLideN64-style with projection compensation), an in-window **FPS overlay** (presented + native game rate, configurable corner/color/on-off, min readable size), and the RecompFrontend launcher UI with a game-specific **Enhancements** tab (overscan / FOV / wave-grid / smooth-water / unlock) |
| **Phase 8** | Release Preparation | Not Started |

> **Playable.** The game boots, the launcher and in-game settings respond to
> keyboard/controller, races run stably start to finish, and music/voices play
> correctly. Controls are fully rebindable through the launcher. A crash in a
> later course is the current known issue (an access violation to a computed
> address; forensics in progress). Residual crashes from a missing indirect-call
> symbol remain fixable in minutes with the included tooling (see `scripts/`).

### Build Statistics

| Metric | Value |
|--------|-------|
| **Executable** | `build/WaveRace64Recomp` (Linux ELF) / `build\WaveRace64Recomp.exe` (Windows, ~12.5 MB, custom embedded app icon) |
| **Build system** | CMake + Ninja |
| **Platforms** | Windows 11 x64 (clang-cl + MSVC environment, D3D12/Vulkan) -- verified; Linux x86_64 (Vulkan/SDL2) -- builds, unverified since Windows changes |
| **Dependencies linked** | RT64, N64ModernRuntime (ultramodern), RecompFrontend (recompui + recompinput), recompiled funcs |
| **Link strategy** | `--start-group`/`--end-group` on GNU linkers; `/FORCE:MULTIPLE` on MSVC-style linkers |

### Known Limitations

- **Overlay relocation data:** still absent, but no longer a blocker -- this game loads
  each overlay at a fixed VRAM slot, and the runtime now tracks overlay residency
  automatically via the PI DMA hook in N64ModernRuntime (`reload_overlays_on_dma`)
- **Controller Pak:** Stub functions only (osPfs*)
- **Symbol map:** JAL-scan derived; indirect-call targets are still being discovered
  during play-testing (automated fix loop: `scripts/bringup_loop.ps1`)
- **Settings UI**: the RecompFrontend launcher (graphics, audio, controls) is
  integrated. Display settings apply live — resolution now renders as a
  **dropdown** (new dropdown-enum config-option type in our librecomp +
  RecompFrontend forks) with Original 1×–9× choices (240p → 2160p/4K) plus
  Auto; MSAA, framerate and fullscreen as before; **controls are fully
  rebindable** (the port routes input through recompinput, so the Controls tab
  actually takes effect). The Graphics **Aspect Ratio** setting is honored
  live (Expand = widescreen, Original = 4:3) and composes with a 3-way
  **Border Area** mode. A game-specific **Enhancements** tab exposes the
  **Border Area** dropdown (Original black borders / Overscan Crop
  full-window default / Extended experimental), camera FOV with a
  real **Reset to 45°** action button, wave-grid size, an **Unlock** button
  (marks every difficulty complete in the EEPROM save — keeps a
  `.unlock_backup` — applied on next game start), a configurable **FPS
  overlay** (on/off, position, color, background opacity — the extra options
  hide while the display is off), and an **experimental Motion Blur**
  slider (present-time accumulation trail, default off). The launcher
  background is an original animated seascape: parallax waves, drifting
  clouds, gliding gulls, pulsing sun. **HD texture packs are mods**: drop an
  `.rtz` (a pack zip with `rt64.json`, optional `mod.json` + `thumb.png` for
  name/author/icon) into `mods/` and it appears in the **Mods tab** with a
  per-pack toggle; multiple enabled packs merge with mod-list order deciding
  per-texture priority (see `textures/README.md`; `WR64_TEXPACK` remains as a
  path/zip env override, `WR64_TEXDUMP` dumps textures for pack authoring).
  **F5** toggles all replacements
  on/off live, any time a pack is loaded. The action buttons use a new
  stateless **Button config-option type** added to our librecomp +
  RecompFrontend forks (the button label no-wrap fix also resolves upstream
  RecompFrontend issue #26). Environment
  variables below remain as overrides/fallbacks and for dev tooling. **Border
  removal is boot-time** — toggling it in the launcher takes effect after a
  restart (the renderer's aspect ratio is fixed at launch; see below)
- **Widescreen rough edges** (needs game patches): objects can pop in at the screen
  edges because the game culls against its original frustum — confirmed NOT to
  follow the camera FOV. HUD/menu handling is solved (per-scene presentation below)
- **Borders removed by default** (`WR64_BORDERS=1` or `show_borders` in
  `wr64_settings.json` restores the stock look; the launcher toggle is hidden
  now that the Overscan Crop supersedes it): the game draws black CRT-overscan
  borders inside its own framebuffer; the port widens the game's scissors
  (top-level and sub-DL), ships the camera FOV widened 45° → 47.75° via recompiler
  instruction patches (5 bisected sites incl. the in-race camera), stretches the
  atmosphere tint over the full frame, and enlarges the detail-water wave grid
  (stock 19×35 → 23×55, `WR64_WAVEGRID`). **Stock mode is boot-time**: it renders
  in RT64 `Original` aspect (native 4:3 with the game's own borders, correct
  proportions, pillarboxed), while border removal uses `Expand`. RT64's aspect
  ratio cannot be flipped safely at runtime (it crashes in-flight queues or
  ghosts stale targets), so the aspect is chosen once at launch and **toggling
  Show Borders requires a restart**. Remaining widescreen seams at extreme aspect
  ratios: the **shore strip** (the game CPU-clips its large ground polygons to the
  original view rect — hunt ongoing) and the water-foam framebuffer effect
  (structurally limited to the original fb region). Full trail in
  `docs/RE-NOTES.md`. Shares its fix with widescreen edge pop-in

### Environment variables & keys

| Setting | Effect |
|---------|--------|
| `WR64_WIDESCREEN=0` | Force original 4:3 aspect (default: expand 3D to the window) |
| `WR64_BORDERS=1` | Restore the original in-framebuffer black borders (boot-time stock mode; the launcher toggle is hidden since the Overscan Crop superseded it — use this env var or `wr64_settings.json`). Border removal is the **default**: scissors (top-level and sub-DL) are widened, the atmosphere tint covers the full frame, the wave grid is enlarged, the HUD keeps proportions in widescreen, and 2D menus present as centered 4:3. Stock (borders-on) mode uses RT64 `Original` aspect and is chosen at launch — changing it needs a restart. Remaining widescreen seams: the shore strip and the water-foam framebuffer effect (see RE-NOTES) |
| `WR64_WAVEGRID=RxC` | Detail-water mesh grid size override (default `23x55`, stock game `19x35`, clamped to `40x96`). Larger grids extend the detailed foam water further into widescreen margins at negligible cost on PC |
| `WR64_SCENE_ASPECT=0` | Disable the per-scene 4:3 menu presentation (keep everything widescreen). Only relevant while borders are removed (default) |
| `WR64_SCENE_DEBUG=1` | Log the scene classifier (`[SCENE] world= menuworld= ... -> wide/menu`) |
| `WR64_WINDOW=WxH` | Startup window size override (e.g. `1600x900`) |
| `WR64_HUD=stretch` | Stretch all 2D (HUD and menus) with the window in widescreen instead of keeping it proportional/centered (default: centered; the centered mode relies on border removal — the default — for RT64's compensation to engage). A third mode — proportional elements anchored to the window edges — needs per-element extended-GBI tagging (future game patches) |
| `WR64_FB_DUMP=1` | Dump the 320x240 framebuffer to `fb_dump*.bin` at ~10/30/50s (`scripts/measure_borders.py`) |
| `WR64_HIGHFPS=1` | Experimental: present at display refresh rate with RT64 transform interpolation between the game's native 20 Hz frames. The former **cloud stutter** artifact is fixed (see `WR64_VTXINTERP` below). See `docs/RE-NOTES.md` |
| `WR64_VTXINTERP=N` | Per-vertex interpolation for small CPU-animated meshes at high FPS — this is what makes the **drifting clouds smooth** instead of snapping at 20 Hz. The value is a max-vertex-count threshold per transform: default **64** (unset or `1`), `0` disables. The threshold keeps the camera-anchored wave mesh (350–870 verts) on its original snapped animation — interpolating it warps the water. `WR64_VTXINTERP_DEBUG=1` logs per-transform velocity stats. See RE-NOTES "Cloud stutter — SOLVED" |
| `WR64_VTXINTERP_RIGID=1` | **Experimental** smooth-water: rigid-translation interpolation of the large wave meshes (also togglable in the launcher **Enhancements → Smooth Water**). Off by default — the wave motion can read wrong; see RE-NOTES "Wave-mesh interpolation" |
| `WR64_OVERSCAN=<0/1/2>` | **Border Area** mode (also a launcher **Enhancements** dropdown, independent of the Graphics **Aspect Ratio** setting): `1` (default) = **Overscan Crop**, a GLideN64-style final-stage crop of the TV-overscan margins with scale-up, so gameplay fills the whole window (1P and 2P split-screen) with the 3D proportions fully compensated; `0` = **Original black borders** (Expand: wide world with original-scale top/bottom bars and black side bars over the expansion-edge artifact zone; 4:3: the native bordered frame); `2` = **Extended** (experimental, raw uncropped frame). See RE-NOTES "Border Area modes" |
| `WR64_FRAME_SIDES=<pct>` | Width of the black side bars per side in Border Area = Original + Expand (default `6`; they are wider than the real border columns on purpose, to cover the world-expansion edge artifacts) |
| `WR64_MOTIONBLUR=<0-90>` | **Experimental** motion blur (also a launcher **Enhancements** slider, default off): present-time accumulation — each frame keeps a fading trail of the previous ones. Moving content trails; the launcher UI stays sharp. Prototype with known visual side effects; see RE-NOTES "Motion blur prototype" |
| `WR64_SHARPEN=<0-100>` | Contrast-adaptive sharpening at the final present (also a launcher **Enhancements** slider, default off) — crispens the upscaled image without ringing halos |
| `WR64_CRT=<0-100>` | **CRT Filter** (also a launcher **Enhancements** slider, default off): Trinitron-style look at the final present — aperture-grille RGB phosphor stripes, scanlines locked to the game's real source lines, slight barrel curvature with rounded corners, mild vignette, **phosphor glow/halation**, and **P22 phosphor color**, all brightness-compensated. Confined to the game area (black bars stay flat; 2P split = one tube) and resolution-adaptive (mask pitch scales with the window, auto-fades below ~3 px/scanline to avoid moiré). The launcher UI stays clean |
| `WR64_CRT_BEZEL=<0/1>` | **CRT Bezel** (also a launcher **Enhancements** toggle, default on): an **overlay-image** TV frame (`assets/crt_bezel.png`, a boxy matte plastic bezel with a beveled inner rim) composited around the picture; the game is shrunk into the frame's cutout and the bezel picks up a soft diffuse reflection of the screen edges. `0` forces off, `1` forces on (overriding the launcher config). When the CRT Filter is on, the bezel opening follows the barrel curvature. See RE-NOTES "CRT Bezel — overlay image" |
| `WR64_CRT_PERSIST=1` | Enable a subtle phosphor-persistence trail (fading highlight afterglow) as part of the CRT Filter — **off by default**, since the underlying accumulation buffer can misbehave across a manual window resize |
| `WR64_INSTANT_PRESENT=0` | Disable low-latency presentation (RT64 PresentEarly, **on by default**: frames present as soon as they're ready instead of waiting for the VI period — less input lag). Use only if you see frame-pacing issues |
| `WR64_DEBUG_LOG=1` | Re-enable the verbose per-frame/per-scene diagnostics (`[RT64-PROJ]`, `[PROJ]`, `[Overlays]` load/unload) that are silenced by default. For RE work; floods the terminal |
| `WR64_POKE_FOV=<factor>` | RE tooling: widen every camera-FOV-shaped value in RDRAM by `<factor>` (e.g. `1.3`, `2.0`). Diagnostic for the border/culling hunt — expect side effects (a second 45° camera-angle field flips the view at high factors). `WR64_POKE_FOV_ONLY=addr[,addr]` restricts to specific addresses; live-read addresses are logged |
| `WR64_DEV=1` | Enable RT64 developer tooling: **F1** inspector (render stats, framebuffer views), F3 raw-RDRAM view, F4 texture replacements. (F2 flips RT64's ray-tracing flag but is non-functional — the RT pipeline is compiled out of modern RT64, see the key table below.) Debug keys are inert without this. |
| `WR64_AUDIO_DUMP=1` | Dump the audio stream to `audio_dump.raw` for analysis (`scripts/analyze_audio_dump.py`) |
| `WR64_TEXPACK=<path>` | Load an RT64 HD texture-replacement pack from `<path>` — a **folder or a `.zip`** (both need `rt64.json` + hash-named images inside) — and enable it; toggle live with **F5**. `WR64_TEXPACK=1` = `./textures`. Texture packs can also be installed as **`.rtz` mods** in `mods/` (Mods tab, per-pack toggles). See `textures/README.md` |
| `WR64_TEXDUMP=<dir>` | Dump every texture RT64 loads (hash-named, Rice/TMEM format) into `<dir>` — raw material for building a pack. `WR64_TEXDUMP=1` = `./textures_dump`. See `textures/README.md` |

Keyboard (defaults): WASD = stick, X = A, Z = B, LShift = Z, Return = START,
arrows = D-pad, Q/E = L/R, IJKL = C-buttons, Esc = open settings menu (during
gameplay; the settings menu has a quit option), F11 / Alt+Enter = fullscreen,
F5 = toggle HD texture replacements (when a pack is loaded), F12 = save a
screenshot of the presented frame to `screenshots/`.

Command line: `--play` skips the launcher and boots straight into the game
(handy for Steam or frontend integration). Crashes write a minidump to
`crash_dumps/` (open with WinDbg/Visual Studio to see the faulting stack).
**All bindings are remappable** in the launcher's Controls tab. Game controllers
map automatically; the window title shows the measured FPS and the selected
target framerate.

### RT64 developer tools

Enable developer mode, then use the debug keys in-game:

```powershell
# Windows (PowerShell)
$env:WR64_DEV = '1'
.\build\WaveRace64Recomp.exe
```

```bash
# Linux
WR64_DEV=1 ./build/WaveRace64Recomp
```

| Key | Tool |
|-----|------|
| **F1** | RT64 Inspector (ImGui): render statistics/profiling, framebuffer views, user & enhancement configuration editors (resolution, aspect, MSAA, filtering -- applied live) |
| **F2** | Toggle ray tracing — **non-functional in current RT64**: the shortcut flips the flag, but every consumer sits behind `#if RT_ENABLED`, which no build defines, and the RT shader pipeline was never ported into the modern RT64 rewrite (it's a leftover from the original SM64RT-era path tracer). No GPU will show a difference |
| **F3** | Toggle raw-RDRAM framebuffer view (shows the game's original 320x240 output; brief artifacts when toggling back are a known RT64 quirk) |
| **F4** | Toggle texture replacements on/off (RT64's own shortcut, dev-mode only; **F5 does the same without dev mode** — see `textures/README.md`) |

Without `WR64_DEV=1` these keys are deliberately inert (RT64 itself only guards F1;
we gate the rest to keep players out of debug views).

> **Note on frame rate:** Wave Race 64 natively runs at ~20 FPS (its game logic is
> built around a 20 Hz update) — the title-bar FPS showing ~20 is authentic console
> behavior, not a performance problem. A 60 FPS enhancement is on the roadmap and
> requires game patches.

### Recompilation Statistics

| Metric | Value |
|--------|-------|
| **Functions recompiled** | 1,281 |
| **Generated C source files** | 23 (`funcs_0.c` -- `funcs_22.c`) |
| **Audio RSP microcode** | recompiled from ROM `0x8DFB0` (`recomp/aspMain.us.rev1.toml` -> `rsp/aspMain.cpp`, generated) |
| **Header file** | `funcs.h` (all function declarations) |
| **Overlay dispatch table** | `recomp_overlays.inl` |
| **Entry point** | `lookup.cpp` (ROM name + entrypoint) |

Symbols were sourced from the [Wave Race 64 decomp project](https://github.com/WACOMalt/Wave-Race-64):
- **756** base functions extracted from decomp symbol files
- **1,228** functions after JAL-target auto-splitting
- **1,281** functions after runtime-discovered indirect-call splits and ROM
  pointer-table scanning (`scripts/find_indirect_targets.py`)

---

## What is this?

This project uses static recompilation to translate Wave Race 64's N64 MIPS binary code into native PC code, enabling the game to run natively on modern hardware with enhancements like:

- Arbitrary resolution support (720p, 1080p, 4K+)
- Widescreen and ultrawide aspect ratios
- Modern controller support (Xbox, PlayStation, Switch Pro, keyboard+mouse)
- Enhanced rendering via [RT64](https://github.com/rt64/rt64)
- 60fps support (planned)

## Prerequisites

### Required
- **Wave Race 64 (USA Rev 1) ROM** -- You must legally own and provide your own ROM file
  - Expected SHA-1: `508dfc2d4caa42b6f6de5263d0aed5e44ac7966a`
  - Place as `baserom.us.rev1.z64` in the project root
- **CMake** >= 3.20
- **C++ Compiler** -- MSVC 2022 (Windows), GCC 12+ (Linux), or Clang 15+
- **Python** >= 3.10

### Platform-Specific
- **Windows:** Visual Studio 2022 (or Build Tools) with C++ workload,
  **Windows SDK 10.0.26100 or newer** (RT64 needs `D3D12_HEAP_TYPE_GPU_UPLOAD`),
  and LLVM/clang -- either the VS Clang component or a
  [portable LLVM](https://github.com/llvm/llvm-project/releases) with `clang` on
  `PATH` (RT64's shader pipeline invokes it). Full walkthrough:
  [docs/WINDOWS-BRINGUP.md](docs/WINDOWS-BRINGUP.md)
- **Linux:** Vulkan SDK >= 1.3, SDL2 development libraries

## Building

```bash
# 1. Clone with submodules
git clone --recurse-submodules https://github.com/WACOMalt/WaveRace64-Recomp.git
cd WaveRace64-Recomp

# 2. Place your ROM in the project root
cp /path/to/your/rom baserom.us.rev1.z64

# 3. Build the N64Recomp toolchain (if not already built)
cd lib/N64Recomp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j$(nproc)
cd ../..

# 4. Run the recompiler to generate RecompiledFuncs/
./lib/N64Recomp/build/N64Recomp recomp/waverace64.toml

# 5. Build the project
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --config Release -j$(nproc)
```

The build produces `build/WaveRace64Recomp` (Linux) or `build/WaveRace64Recomp.exe` (Windows).

## Running the Recompiler

The static recompilation step has already been completed. To reproduce it:

```bash
# 1. Place your ROM in the project root
cp /path/to/your/rom baserom.us.rev1.z64

# 2. Build the N64Recomp toolchain (if not already built)
cd lib/N64Recomp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release -j$(nproc)
cd ../..

# 3. Run the recompiler
./lib/N64Recomp/build/N64Recomp recomp/waverace64.toml
```

The recompiler reads `baserom.us.rev1.z64` and `recomp/waverace64.us.rev1.syms.toml`, then outputs recompiled C code to `RecompiledFuncs/`.

## Releases & Distribution

A prebuilt binary can be distributed **without ever shipping any Nintendo
content** — the same model used by [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp),
[BanjoRecomp](https://github.com/BanjoRecomp/BanjoRecomp), and the other
N64Recomp ports. Two facts make this work:

- **The binary contains recompiled *code*, never game *assets*.** N64Recomp
  statically translates the game's MIPS code to C at build time, and that C is
  compiled into the executable. The recompiled C (`RecompiledFuncs/`) is
  generated during the build and is **not committed** to the repo. Game
  *assets* — textures, audio, models, level and course data, text — are never
  copied out of the ROM and never end up in the binary or the release.
- **Assets are read from the user's own ROM at runtime.** The player supplies
  their own legally-obtained Wave Race 64 (USA Rev 1) ROM through the launcher
  (**Start / Load ROM**). The launcher validates the SHA-1, stores the ROM, and
  loads every asset from it. A downloaded release with no ROM prompts for one on
  first launch — nothing is playable until the user provides their copy.

A release archive therefore contains only: the executable, its runtime DLLs
(SDL2, dxcompiler, dxil, and the MSVC runtime on Windows), and the port's *own*
`assets/` (launcher art, fonts, controller database). No ROM, no game assets.

**Building still requires the ROM.** The recompiler must read it to generate the
C, so any build machine needs the ROM. WR64's symbols
(`recomp/waverace64.us.rev1.syms.toml`) are already public and committed, so the
ROM is the only private input required.

## Project Structure

```
WaveRace64-Recomp/
|-- CMakeLists.txt                  # Root build configuration
|-- README.md                       # This file
|-- LICENSE                         # GPLv3
|
|-- lib/                            # Git submodules
|   |-- N64Recomp/                  # Static recompiler (MIPS -> C)
|   |-- rt64/                       # GPU rendering engine (F3D -> D3D12/Vulkan)
|   +-- N64ModernRuntime/           # Runtime library (OS stubs, threading)
|
|-- recomp/                         # N64Recomp configuration
|   |-- waverace64.toml             # Recompiler main config
|   |-- waverace64.us.rev1.syms.toml  # Symbol definitions (1,228 functions)
|   +-- overlays.us.rev1.txt        # Overlay address table
|
|-- RecompiledFuncs/                # Generated by N64Recomp (not committed)
|   |-- funcs_0.c -- funcs_21.c    # 22 recompiled C source files (~19.5MB)
|   |-- funcs.h                     # Function declarations header (65KB)
|   |-- lookup.cpp                  # Entrypoint + ROM name
|   +-- recomp_overlays.inl         # Overlay dispatch table (90KB)
|
|-- scripts/                        # Build and utility scripts
|   |-- generate_symbols.py         # Extract symbols from decomp -> TOML format
|   |-- auto_split_functions.py     # JAL-scan to discover sub-functions
|   +-- find_missing_jal_targets.py # Detect missing call targets in ROM
|
|-- src/                            # Game-specific source code
|   |-- main.cpp                    # Application entry point
|   |-- rt64_render_context.cpp     # RT64 rendering integration
|   +-- os_stubs.cpp                # N64 OS function stub implementations
|
|-- assets/                         # Non-copyrighted application assets
+-- docs/                           # Documentation
```

## Development Progress

### Phase 1: Environment Setup & Toolchain (COMPLETE)
- [x] Created `WACOMalt/WaveRace64-Recomp` repository on GitHub
- [x] Added N64Recomp, RT64, N64ModernRuntime as git submodules
- [x] Created CMakeLists.txt for the project
- [x] Built N64Recomp toolchain from source
- [x] Exported symbols from decomp project into N64Recomp-compatible TOML format
- [x] Created `generate_symbols.py` -- extracts 756 base functions from decomp symbol files
- [x] Created `auto_split_functions.py` -- JAL scanning expanded 756 -> 1,228 functions
- [x] Created `find_missing_jal_targets.py` -- validates all call targets are covered

### Phase 2: Static Recompilation (COMPLETE)
- [x] Created `waverace64.toml` configuration for N64Recomp
- [x] Defined all 19 overlays with ROM/VRAM addresses
- [x] Fixed entrypoint (0x80000400 -> 0x80046800)
- [x] Fixed `__osException` merge issue (two symbol files defining the same function)
- [x] Fixed auto-split function boundaries for edge cases
- [x] Successfully recompiled all 1,228 functions to C
- [x] Generated overlay dispatch table (`recomp_overlays.inl`)
- [x] All generated code compiles cleanly

### Phase 3: Runtime Integration (COMPLETE)
- [x] Set up main.cpp with ultramodern runtime initialization
- [x] Configured RT64 render context for Linux (SDL_Window* passthrough)
- [x] Fixed `get_game_thread_name()` API signature (`const OSThread* t`)
- [x] Added RT64 internal include paths and compile definitions to CMakeLists.txt

### Phase 4: Build & Link (COMPLETE -- First Successful Build)
- [x] Re-ran N64Recomp to regenerate RecompiledFuncs (fixed truncated funcs_21.c)
- [x] Disabled `relocatable_sections_path` in waverace64.toml (overlay sections lack relocation data)
- [x] Added os_stubs.cpp with stub implementations for missing N64 OS functions
- [x] Used `--start-group`/`--end-group` for circular link dependencies
- [x] Build produces 11 MB WaveRace64Recomp ELF executable

### Phase 5: Audio & Input (COMPLETE)
- [x] Audio via statically recompiled RSP microcode (LLE, not SM64-derived HLE):
      located in ROM by runtime OSTask instrumentation, recompiled with RSPRecomp,
      16-entry command dispatch table (`recomp/aspMain.us.rev1.toml`)
- [x] Audio output pacing fixes (frames-vs-samples unit bug; device reopen on
      frequency change -- game runs at 32 kHz)
- [x] SDL2 controller + keyboard mapping (N64 -> modern gamepad) -- verified in-game
- [x] EEPROM save -> file-based save redirect (librecomp default; `saves/` created)

### Phase 6: Game-Specific Fixes (IN PROGRESS)
- [x] Overlay residency tracking (automatic, via PI DMA hook in N64ModernRuntime --
      see the `overlay-dma-autoload` submodule branch)
- [x] Attract mode stable (2,342+ frames crash-free)
- [x] Menu navigation verified with keyboard
- [ ] Full gameplay testing (all modes, courses, riders) -- ongoing; use
      `scripts/bringup_loop.ps1` to auto-fix missing-symbol crashes
- [ ] Water rendering verification

### Phases 7-8: Enhancements & Release
- [x] Widescreen (3D expand; per-scene presentation keeps 2D menus at 4:3)
- [x] High-FPS presentation (`WR64_HIGHFPS=1`, interpolated; cloud stutter fixed via `WR64_VTXINTERP`)
- [ ] Border removal endgame (culling frustum + wave-grid, see `docs/RE-NOTES.md`)
- [x] HD texture support (`.rtz` texture-pack mods in the Mods tab, Textures tab path/zip, F5 live toggle)
- [ ] Release packaging

## Tools

| Script | Purpose |
|--------|---------|
| [`generate_symbols.py`](scripts/generate_symbols.py) | Extracts function symbols from the decomp project's linker scripts and converts them to N64Recomp TOML format |
| [`auto_split_functions.py`](scripts/auto_split_functions.py) | Scans the ROM binary for JAL instructions to discover function boundaries not in the decomp symbol files (756 -> 1,228) |
| [`find_missing_jal_targets.py`](scripts/find_missing_jal_targets.py) | Validates that all JAL call targets in the ROM are covered by symbol definitions |
| [`find_indirect_targets.py`](scripts/find_indirect_targets.py) | Scans ROM data for function-pointer-table targets missed by JAL scanning; validates against `jr $ra` boundaries and filters switch jump tables. `--apply` for batch splits, `--split 0xADDR` for one crash address |
| [`bringup_loop.ps1`](scripts/bringup_loop.ps1) | Unattended crash-driven loop: run the game, split the symbol at any "Failed to find function" address, regenerate, rebuild, repeat |
| [`audio_ucode_loop.ps1`](scripts/audio_ucode_loop.ps1) | Same loop for the audio microcode's "Unhandled jump target" errors (note: those print to **stdout**) |
| [`drive_to_menu.ps1`](scripts/drive_to_menu.ps1) | Autonomous UI test driver: boots the game and navigates title → watercraft select, saving per-step screenshots and stderr telemetry to `drive_out/`. Input is posted straight to the game window's message queue (no focus steal — the game can sit behind other windows). Combine with `WR64_WINDOW=1920x800` to reproduce ultrawide layout bugs without manual testing |
| [`drive_to_race.ps1`](scripts/drive_to_race.ps1) | Autonomous race driver: boots the game, navigates title → Time Trials → Sunny Beach → live race and holds the accelerator, capturing screenshots + telemetry. Same no-focus-steal input as above. The workhorse behind the FOV-site classification sweeps |
| [`classify_fov_sites.py`](scripts/classify_fov_sites.py) / [`classify_fov_sites_race.py`](scripts/classify_fov_sites_race.py) | Per-site camera-FOV bisection: patch one inline-45.0f site at a time (via `[[patches.instruction]]`), rebuild, probe attract demos / a live race, and classify by RT64 projection telemetry |
| [`probe_fov_group.py`](scripts/probe_fov_group.py) | Group-bisection variant: patch an arbitrary set of FOV sites per run — found the in-race camera site in ~6 runs instead of 28 |
| [`decode_texture_dump.py`](scripts/decode_texture_dump.py) | Decode a `WR64_TEXDUMP` texture dump into viewable PNGs, labeled contact sheets (`_index_*.png`), a hash→format listing, and a directly-loadable RT64 pack (`png/` + `rt64.json`). Handles this runtime's 32-bit-word byte-swap and the CI8/RGBA16/RGBA32/IA/I formats. See `textures/README.md` |

## Related Projects

| Project | Description |
|---------|-------------|
| [N64Recomp](https://github.com/N64Recomp/N64Recomp) | The static recompilation tool |
| [RT64](https://github.com/rt64/rt64) | N64 rendering engine for PC |
| [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) | Runtime support library |
| [Wave-Race-64 Decomp](https://github.com/WACOMalt/Wave-Race-64) | Our decomp fork (symbol source) |
| [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) | Reference implementation (Majora's Mask) |

## Legal

This project and any binaries built from it do **not** contain any Nintendo
copyrighted assets — no textures, audio, models, level/course data, or text.
Those are read at runtime from a ROM you must **legally own and provide
yourself**. The repository does not include the ROM, and the generated
recompiled C (`RecompiledFuncs/`) is not committed. This is the same
distribution model used by other N64Recomp ports (Zelda64Recomp, BanjoRecomp);
it has not been tested in court and is provided without warranty — use it with
your own copy of the game.

## License

GPLv3 -- see [LICENSE](LICENSE) for details.
