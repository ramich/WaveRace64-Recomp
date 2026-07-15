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

## Current Status

| Phase | Description | Status |
|-------|-------------|--------|
| **Phase 1** | Environment Setup & Toolchain | **COMPLETE** |
| **Phase 2** | Static Recompilation | **COMPLETE** |
| **Phase 3** | Runtime Integration | **COMPLETE** |
| **Phase 4** | Build & Link (RT64 + Runtime) | **COMPLETE** |
| **Phase 5** | Audio & Input | **COMPLETE** -- input verified by hand, audio via recompiled RSP microcode at 32 kHz |
| **Phase 6** | Game-Specific Fixes | **IN PROGRESS** -- attract mode fully stable; races being play-tested |
| **Phase 7** | Enhancements | **IN PROGRESS** -- widescreen (RT64 Expand), FPS counter, dev inspector |
| **Phase 8** | Release Preparation | Not Started |

> **Playable (early).** The game boots, renders attract mode stably, menus respond to
> keyboard/controller, and music/voices play correctly. Remaining crashes during
> gameplay are missing indirect-call symbols, fixable in minutes with the included
> tooling (see `scripts/`).

### Build Statistics

| Metric | Value |
|--------|-------|
| **Executable** | `build/WaveRace64Recomp` (Linux ELF) / `build\WaveRace64Recomp.exe` (Windows, ~8 MB) |
| **Build system** | CMake + Ninja |
| **Platforms** | Windows 11 x64 (clang-cl + MSVC environment, D3D12/Vulkan) -- verified; Linux x86_64 (Vulkan/SDL2) -- builds, unverified since Windows changes |
| **Dependencies linked** | RT64, N64ModernRuntime (ultramodern), recompiled funcs |
| **Link strategy** | `--start-group`/`--end-group` on GNU linkers; `/FORCE:MULTIPLE` on MSVC-style linkers |

### Known Limitations

- **Overlay relocation data:** still absent, but no longer a blocker -- this game loads
  each overlay at a fixed VRAM slot, and the runtime now tracks overlay residency
  automatically via the PI DMA hook in N64ModernRuntime (`reload_overlays_on_dma`)
- **Controller Pak:** Stub functions only (osPfs*)
- **Symbol map:** JAL-scan derived; indirect-call targets are still being discovered
  during play-testing (automated fix loop: `scripts/bringup_loop.ps1`)
- **No settings UI yet** (settings via environment variables, see below)
- **Widescreen rough edges** (needs game patches): the HUD stretches with the window
  instead of staying at its original aspect, and objects can pop in at the screen
  edges because the game culls against the 4:3 frustum
- **Black borders** around the game image — drawn by the game itself inside its
  320x240 framebuffer (its own overscan compensation; RT64 presents edge-to-edge,
  verified via VI viewport instrumentation). Removing them needs a game patch that
  widens the game's internal viewport, same as Pilotwings64Recomp's border patch

### Environment variables & keys

| Setting | Effect |
|---------|--------|
| `WR64_WIDESCREEN=0` | Force original 4:3 aspect (default: expand 3D to the window) |
| `WR64_DEV=1` | Enable RT64 developer tooling: **F1** inspector (render stats, framebuffer views), F2 ray tracing, F3 raw-RDRAM view, F4 texture replacements. Debug keys are inert without this. |
| `WR64_AUDIO_DUMP=1` | Dump the audio stream to `audio_dump.raw` for analysis (`scripts/analyze_audio_dump.py`) |

Keyboard: WASD = stick, X = A, Z = B, LShift = Z, Return = START, arrows = D-pad,
Q/E = L/R, IJKL = C-buttons, Esc = quit. Game controllers map automatically; the
window title shows the game FPS.

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
| **F2** | Toggle ray tracing (experimental RT64 feature) |
| **F3** | Toggle raw-RDRAM framebuffer view (shows the game's original 320x240 output; brief artifacts when toggling back are a known RT64 quirk) |
| **F4** | Toggle texture replacements |

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
- [ ] Widescreen, 60fps, HD texture support
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

## Related Projects

| Project | Description |
|---------|-------------|
| [N64Recomp](https://github.com/N64Recomp/N64Recomp) | The static recompilation tool |
| [RT64](https://github.com/rt64/rt64) | N64 rendering engine for PC |
| [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) | Runtime support library |
| [Wave-Race-64 Decomp](https://github.com/WACOMalt/Wave-Race-64) | Our decomp fork (symbol source) |
| [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) | Reference implementation (Majora's Mask) |

## Legal

This project does **not** contain any Nintendo copyrighted code or assets. You must provide your own legally-obtained ROM file to use this software.

## License

GPLv3 -- see [LICENSE](LICENSE) for details.
