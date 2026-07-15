# Windows Bring-Up Report — WaveRace64-Recomp

**Date:** 2026-07-14/15
**Result:** First successful Windows build, first-ever launch, **and first interactive
play of this port**. The game boots, renders attract mode stably (2,300+ frames /
2+ minutes crash-free), and **keyboard menu navigation is confirmed working** —
upstream's "Phase 5: Audio & Input" was already half-done: the SDL2 input layer
existed and works; only audio remains stubbed.

Progress since the initial report (sections below describe the original state):

1. **Overlay support implemented and verified** — the original blocker. Added
   `reload_overlays_on_dma()` to the N64ModernRuntime submodule
   (`librecomp/src/overlays.cpp`, hooked from `do_rom_read` in `pi.cpp`): when a DMA's
   source exactly matches a registered code section's ROM address, sections occupying
   the destination are unloaded (overlap-based, avoiding the partial-unload assert for
   the differently-sized overlays sharing the 0x802C5800 slot) and the new section is
   loaded. Run logs confirm `.codeseg` relocating to its true address and `ovl_*`
   sections paging through the slot. Submodule is dirty — fork + upstream PR needed.
2. **~50 symbol-map fixes** in `recomp/waverace64.us.rev1.syms.toml`: one batch of 38
   dispatch-handler splits found by scanning ROM data for pointer tables
   (`scripts/find_indirect_targets.py`, with a jump-table filter — stride-4 pointer
   runs into a single function are switch tables and must NOT be split), plus ~12
   crash-driven splits found by the automated loop (`scripts/bringup_loop.ps1`:
   probe → split at the failing address, jr-$ra-validated → regenerate → rebuild).
   Crash-free frames progressed 0 → 162 → 973 → 2,342 over the session.
3. **Gotcha:** after the recompiler adds output files, CMake configure must re-run
   (`file(GLOB)` staleness) or the link fails with undefined `func_*` symbols. The
   loop script does this automatically.

4. **Audio implemented and confirmed working** (user-verified by ear, correct pitch,
   no static). The work: located the audio microcode via runtime instrumentation of
   the M_AUDTASK OSTask (ucode at ROM `0x8DFB0`; the task's `ucode_size=0x800` field
   under-reports — real text is `0xE20`, same build as Pilotwings' aspMain);
   recompiled it with RSPRecomp (`recomp/aspMain.us.rev1.toml`, 16 dispatch targets —
   4 runtime-discovered via `scripts/audio_ucode_loop.ps1`, 12 transplanted from
   Pilotwings' matching list); fixed two bugs in `src/audio.cpp`:
   - `audio_get_frames_remaining` returned samples where the runtime expects frames
     (2× over-report → underrun crackle), plus added the anti-pop lookahead margin.
   - `audio_set_frequency` updated `current_frequency` before `ensure_audio_device`
     compared against it → the device never reopened on rate change and stayed at the
     startup dummy 48000 Hz while the game generated at 32000 Hz (fast/high-pitched
     audio + crackle). Fixed by tracking the device's actual rate separately.
   The game runs its audio at **32000 Hz** (visible as the second "Audio device
   opened" line).
   NOTE: the recompiled ucode's "Unhandled jump target" diagnostic prints to STDOUT,
   not stderr — capture both when hunting targets.

**Next:** play-test races to flush remaining missing handlers (each is a ~3-minute
automated fix via the loop scripts), then the patches pipeline for enhancements
(widescreen), an ImGui settings overlay, and the upstream PRs (Windows fixes +
overlay support + syms + audio).

This document records what was done, the exact current state, and the recommended next
steps. Upstream baseline: commit `61cb264` (June 2, 2026), which had only ever been
built on Linux and never executed.

---

## 1. Environment

| Component | Version / source |
|-----------|------------------|
| OS | Windows 11 Pro |
| Toolchain | VS Build Tools 2022 (17.7), MSVC 14.37, bundled CMake + Ninja |
| **Windows SDK** | **10.0.26100 required** (RT64 uses `D3D12_HEAP_TYPE_GPU_UPLOAD`; 22621 fails) |
| Compiler for the app | `clang-cl` from portable LLVM 19.1.3 ([llvm-project releases](https://github.com/llvm/llvm-project/releases/tag/llvmorg-19.1.3)) |
| `clang` on PATH | Also from portable LLVM — RT64's shader pipeline invokes plain `clang` to preprocess `.hlsli` files |

All build commands run inside a `vcvars64.bat` environment with the portable LLVM `bin`
appended to `PATH`. (See Pilotwings64Recomp's `BUILDING-WINDOWS.md` for the general
Windows recomp toolchain setup this reuses.)

## 2. What was done, in order

### 2.1 Submodules and toolchain

```powershell
git submodule update --init --recursive
# Build the pinned N64Recomp (plain MSVC is fine for the tools):
cmake -S lib/N64Recomp -B lib/N64Recomp/build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build lib/N64Recomp/build --config Release -j 12
```

### 2.2 ROM preparation — byte order matters

The provided dump was **byte-swapped** (`.v64`): header `37 80 40 12`, internal name
garbled as `AWEVR CA E46`. Converted by swapping every byte pair; original kept as
`baserom.us.rev1.v64`. After conversion:

- Header `80 37 12 40`, internal name `WAVE RACE 64`
- SHA-1 `508dfc2d4caa42b6f6de5263d0aed5e44ac7966a` — **exact match** for the USA Rev 1
  hash required by the README.

### 2.3 Recompilation

```powershell
.\lib\N64Recomp\build\N64Recomp.exe recomp\waverace64.toml
```

Output: **1,231 functions** across 25 files in `RecompiledFuncs/`. The recompiler
printed repeated warnings that foreshadowed the runtime blocker:

```
[Info] Ambiguous jal target 0x802C5800 in function func_80092CF0, falling back to function lookup
[Info] Ambiguous jal target 0x802C5F6C in function func_80092CF0, falling back to function lookup
```

`0x802C5800` is a VRAM address where **two different overlay functions overlap** (the
syms file defines e.g. `func_1B1FB0_802C5800` and a second function at the same
address in another overlay) — resolvable only at runtime by knowing which overlay is
loaded.

### 2.4 Windows fixes applied (all local, uncommitted)

**`CMakeLists.txt`:**

1. `find_package(SDL2 REQUIRED)` guarded to non-Windows; on Windows, SDL2 is fetched as
   the official `SDL2-devel-<ver>-VC.zip` via FetchContent (pattern proven in
   Pilotwings64Recomp), with `SDL2.dll`/`dxil.dll`/`dxcompiler.dll` copied next to the
   exe post-build.
2. The VC package puts headers in `include/` but sources use `<SDL2/SDL.h>` (Linux
   layout) — headers are mirrored into `<builddir>/sdl2-compat-include/SDL2/` at
   configure time and that dir added to the include path.
3. `-Wl,--start-group`/`--end-group` and `--allow-multiple-definition` guarded to
   non-MSVC; the MSVC branch links plainly (MSVC-style linkers resolve static-lib cycles
   iteratively) with `/FORCE:MULTIPLE`.
4. `NOMINMAX` defined for the game target — `windows.h`'s `min`/`max` macros break
   hlslpp (via RT64 headers).
5. `-Wall -Wextra` guarded to non-MSVC (clang-cl maps `-Wall` to MSVC `/Wall` ≈
   `-Weverything`, drowning the log).
6. `-D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` on WIN32 (vcredist mutex issue, same as
   Pilotwings).

**`src/main.cpp`:**

7. `create_window()` — on Windows, `ultramodern::renderer::WindowHandle` is
   `{HWND, DWORD thread_id}`, not `SDL_Window*`. Added `SDL_GetWindowWMInfo` +
   `GetCurrentThreadId()` construction, and dropped `SDL_WINDOW_VULKAN` from the window
   flags on Windows (RT64 attaches via the native HWND and picks D3D12/Vulkan itself).

**`src/rt64_render_context.cpp`:**

8. `<windows.h>`, `<unknwn.h>`, `<objidl.h>` included before RT64 headers on WIN32 —
   `dxcapi.h` requires the COM base declarations (`IUnknown`, `IStream`).

### 2.5 Build and first run

```powershell
cmake -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=<llvm>/bin/clang-cl.exe -DCMAKE_CXX_COMPILER=<llvm>/bin/clang-cl.exe `
  -DCMAKE_MAKE_PROGRAM=ninja -G Ninja `
  "-DCMAKE_CXX_FLAGS=-Xclang -fexceptions -Xclang -fcxx-exceptions" -S . -B build
cmake --build build --config Release -j 12
```

Produces `build/WaveRace64Recomp.exe` (8.2 MB) + `SDL2.dll`, `dxcompiler.dll`,
`dxil.dll`.

**Running:** librecomp resolves the ROM as `waverace64.z64` in the config path, which
`main.cpp` anchors to the current working directory:

```powershell
Copy-Item baserom.us.rev1.z64 waverace64.z64
.\build\WaveRace64Recomp.exe   # run from the repo root
```

Observed output of the first launch:

```
[WR64-RT64] send_dl: ucode=0x00D2380 ucode_data=0x00EE310 data_ptr=0x01388D0
[WR64-RT64] send_dl: DONE
[WR64-RT64] send_dl: ucode=0x00D2380 ucode_data=0x00EE310 data_ptr=0x011F8E8
Failed to find function at 0x802C5800
```

Exit code `0xC0000409` (fail-fast abort). A `saves/` directory was created, confirming
the save subsystem also engaged.

## 3. Current state

**Working (verified by execution):** runtime init, ROM hash validation, thread startup,
RSP microcode hookup, recompiled game code execution, display list submission to RT64,
save path creation. The Windows build itself is fully repeatable.

**The blocker (single, precisely located):** overlay support. The game paged in overlay
code and jumped to VRAM `0x802C5800`; two recompiled functions exist at that address
(one per overlapping overlay) and the runtime cannot tell which is loaded because:

- `main.cpp`'s section table registers 19 code sections with `.relocs = nullptr`
- `relocatable_sections_path` was disabled in `waverace64.toml` (Phase 4 upstream note:
  "overlay sections lack relocation data")
- No overlay load tracking ties the game's DMA loads to librecomp's section table.

**Not started (upstream Phase 5):** audio (stubs), input (stubs), Controller Pak.

## 4. How to proceed

### 4.1 Upstream the Windows support (small, ready now)

The 8 fixes above are guarded and don't disturb the Linux path. Submit as a PR to
`WACOMalt/WaveRace64-Recomp` titled "Windows build support" — it gives the project its
first working Windows build and its first runtime trace. Include the run log; the
author has never seen the port execute.

### 4.2 Overlay support (the real work, unblocks gameplay)

This is the highest-value technical task, with a fast test loop (the crash reproduces
within ~2 seconds of launch):

1. **Understand the runtime contract:** study how `librecomp` overlay loading works in
   `lib/N64ModernRuntime/librecomp` (section registration, `load_overlays` /
   ROM-address matching on DMA) and how a working consumer wires it — Zelda64Recomp is
   the reference (its syms repo carries per-overlay `relocs` arrays and ROM addresses).
2. **Map Wave Race's overlay system:** instrument the recomp's DMA path (`os_stubs.cpp`
   / librecomp's `osPiStartDma` handling) to log ROM offset → RDRAM address → size for
   every DMA. Cross-reference with `recomp/overlays.us.rev1.txt` (19 overlays,
   `.ovl_i*` / `.segment_*` naming = ROM offsets). This yields which overlay is
   resident at `0x802C5800` at crash time.
3. **Produce relocation data:** the decomp (`LLONSIT/Wave-Race-64` or the
   `WACOMalt/Wave-Race-64` fork) may already have splat-relocated overlay sections;
   otherwise relocs must be extracted from the overlay MIPS code (standard splat
   `.rel` handling). Re-enable `relocatable_sections_path` in `waverace64.toml`,
   regenerate with N64Recomp, and populate the `code_sections[].relocs` tables in
   `main.cpp`.
4. **Validate:** the ambiguous-jal warnings at `0x802C5800`/`0x802C5F6C` should
   disappear or resolve at runtime; the game should progress past the current crash —
   next likely stops are the audio/input stubs (Phase 5).

### 4.3 After overlays

Follow upstream's own phase plan: audio (the SM64-derived HLE route noted in their
README, or ultramodern's audio interface like Pilotwings), SDL2 input mapping, EEPROM
save wiring (already partially engaging), then gameplay testing across courses.

## 5. Local artifacts inventory

| File | Status |
|------|--------|
| `CMakeLists.txt`, `src/main.cpp`, `src/rt64_render_context.cpp` | Modified (the 8 fixes) — uncommitted |
| `baserom.us.rev1.z64` | Converted big-endian ROM (verified SHA-1) |
| `baserom.us.rev1.v64` | Original byte-swapped dump (backup; both gitignored) |
| `waverace64.z64` | Copy of the z64 ROM where librecomp looks for it |
| `RecompiledFuncs/` | Generated (1,231 functions; not committed, per upstream) |
| `build/` | Ninja build tree; exe + runtime DLLs |
| `build.log`, `configure.log`, `run_stdout.log`, `run_stderr.log` | Session logs |
| `saves/` | Created by the first run |
