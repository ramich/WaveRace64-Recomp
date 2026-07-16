# Reverse-Engineering Notes

Working notes for game patches. Addresses are USA Rev 1 unless stated.

## Screen / borders

- The game scissors rendering to ~(8,20)-(310,218) (G_SETSCISSOR built by 20+
  functions across scene overlays). Fixed at runtime by rewriting near-fullscreen
  scissors in the display list (`send_dl`). Split-screen scissors (e.g. y0=122 at
  `0x801F0B08`) must not be touched.
- 3D viewport structs (Vp) are all full-screen 320x240; the Vp array lives at
  `0x800DA8C0` (.main data), more instances at `0x80226340`, `0x8023D1B8`,
  `0x80297E18`, `0x80371EE8`.
- Display/screen-mode variable at **`0x800E8170`** (checked by the DL-builder
  functions; values seen: 0, 2, 7). Likely selects 1P/2P/menu screen layout.

## Widescreen (community GameShark codes, mined from GameGenie/AssemblerGames)

The game's aspect-ratio constants are floats (1.3333 = 0x3FAAAAAB); the published
NTSC-U widescreen code patches their high halfwords to 0x3FE3 (1.7777 = 16:9):

- `0x800699FA` (halfword; the float is at `0x800699F8`) — .main
- `0x801E11F2` (float at `0x801E11F0`) — .codeseg
- `0x801EF3F2` (float at `0x801EF3F0`) — .codeseg
- `0x801F07E6` (float at `0x801F07E4`) — .codeseg
- Overlay-slot code patches guarded by which-overlay checks (GameShark `D1`
  conditionals on `0x802C5896`/`0x802C58C2` contents):
  - if `[0x802C5896]==0x8C50`: write `0x3C07` at `0x802C5890`, `0x3FAA` at `0x802C5892`
  - if `[0x802C58C2]==0x4204`: write `0x3FAA` at `0x802C58C6`
- We currently do widescreen via RT64 `AspectRatio::Expand` instead (no game
  patch); these addresses matter for the **edge-culling fix**: the culling code
  likely uses the same aspect/projection constants. Investigate the functions
  referencing these addresses.

## Framerate

- The game logic runs at a hard-coded ~20 Hz. **No working 60fps GameShark code
  exists** (Project64 community consensus: "hard-coded", "would be a dream").
- Recomp-native path: RT64 transform interpolation at display refresh rate
  (`WR64_HIGHFPS=1`, experimental) — the Zelda64Recomp approach, no logic change.
  **User-verified working** (2026-07-15): motion is smooth at display rate.
- **Known artifact: stuttering clouds — CONFIRMED STRUCTURAL (2026-07-15).**
  RT64 interpolates by matching *transforms* (worldTransforms + RigidBody lerp,
  rt64_game_frame.cpp). Billboard clouds regenerate their vertex data per game
  frame under a static transform, and RT64's vertex-level velocity interpolation
  is an unimplemented TODO (rt64_game_frame.cpp ~1012: "TODO: Compute the velocity
  buffer"). No config or tagging fixes this today. Real fix routes, both major:
  (a) implement vertex-velocity interpolation in RT64 (upstream contribution that
  would benefit every recomp), or (b) game-patch the cloud renderer to draw
  matrix-transformed quads instead of world-space billboards (requires finding the
  sky renderer). Parked as a documented limitation of WR64_HIGHFPS.
- True logic-rate change would need the physics timestep and frame counters
  found and patched — major RE effort.

## View culling (the real border/widescreen blocker)

User-verified behavior with the scissor rewrite active: the revealed margins show
ONLY the flat background ocean — islands, racers, and the detailed wave mesh are
absent, and there is a visible color seam at the original view-rect boundary. So the
game culls objects AND sizes its detailed water against the original view rect;
revealing more canvas without widening those bounds shows a half-rendered world.
Border removal is therefore default-off (WR64_BORDERS=0 to experiment) until the
culling bounds are widened. The same fix unlocks clean widescreen (edge pop-in).

Dead ends eliminated (static analysis):
- The view-bounds-shaped globals found by RDRAM scan (0x8037117C {8,224,...},
  0x80371AD4 {8,312,...}) have ZERO absolute-address references in code — the
  0x8037 page is heap; structs are reached via pointers only.
- No float screen-bound constants (312.0f/310.0f/218.0f/302.0f/198.0f) appear as
  lui immediates anywhere — culling is integer-based or computed.

Bisection harness results (scripts/bisect_culling.py, native poke engine in
rt64_render_context.cpp):
- s16 view-rect pairs {8|20, 310..312|217..224}: 14 candidates, widening all gave
  no margin improvement.
- PW64-style f32 clip-plane quads [-x,+x,-y,+y]: zero hits — WR64's camera is not
  PW64-shaped.
- 1.3333f (0x3FAAAAAB) aspect constants: zero hits in all of RDRAM — the community
  GameShark widescreen addresses likely target Rev 0; our ROM is Rev A.
- Static immediates: code compares against 320/240 (full screen), not the inner
  rect — culling is not screen-rect based.

**CAMERA MODEL CRACKED (2026-07-15):** RT64's dialect-aware matrix decoder
(temporary instrumentation in rt64_rsp.cpp matrixCommon) revealed the live
projection: **guPerspective(fovy=45°, aspect=4:3)** — m11 = cot(22.5°) exactly,
loaded via segment 3 (per-frame DL buffer at phys ~0x12D8F0). A second ~75° camera
alternates during demos. Key findings from FOV poking (`WR64_POKE_FOV=1`, scans
for 45.0f/75.0f floats with periodic rescan since camera structs spawn per scene):

- **Widening the camera fovy widens the view** (user-confirmed). **Object culling
  does NOT follow** (user-confirmed decisively: with the FOV widened, buoys never
  appear outside the original view region). Unified model fitting all evidence:
  the projection reads fovy from the pokable camera struct, but **the culling code
  computes its own frustum from the inline 45.0f code constants** (`lui reg,0x4234`)
  — unreachable by memory pokes, patchable only as instructions. Same number, two
  homes: one in data (projection), one in instructions (culling). ✔/✘

  **Site classification (2026-07-15, scripts/classify_fov_sites.py — one site
  patched to 66.5° per run, verdict via RT64-PROJ m11 telemetry):**

  | handler | vram | projection mover |
  |---|---|---|
  | func_8009B910 | 0x8009B93C | **YES** |
  | func_8009BA14 | 0x8009BA40 | no (camera not active in window, or non-projection) |
  | func_8009BAE0 | 0x8009BB0C | no (same caveat) |
  | func_8009BB98 | 0x8009BBC4 | **YES** |
  | func_8009BCA4 | 0x8009BCD0 | **YES** |

  Culling hypotheses eliminated since: precomputed half-angle trig constants
  (tan/cos/sin of 22.5° — zero inline hits); the 0x43C1 "focal" hit at 0x80082A68
  is a plain 386.0 threshold compare (ambiguous, inside 0x22F4-byte func_80081CC8).
  Also present: an army of ~18 stride-0x30 `$a3` inline-45.0 setters at
  0x8009BED8..0x8009C1B0 (per-course cameras?) — unclassified.

  **Round 2 (2026-07-16, same script/method, SITES extended to the two
  standalone sites + the full stride-~0x30 army):**

  | handler | vram | projection mover |
  |---|---|---|
  | func_80089C08 | 0x80089C50 | no (camera not active in window) |
  | func_8009B19C | 0x8009B1C8 | **YES** |
  | func_8009BEBC | 0x8009BED8 | no (same caveat) |
  | func_8009BEEC | 0x8009BF08 | no |
  | func_8009BF1C | 0x8009BF38 | no |
  | func_8009BF4C | 0x8009BF68 | no |
  | func_8009BF7C | 0x8009BF98 | no |
  | func_8009BFAC | 0x8009BFC8 | no |
  | func_8009BFDC | 0x8009BFF8 | no |
  | func_8009C00C | 0x8009C028 | no |
  | func_8009C044 | 0x8009C060 | no |
  | func_8009C074 | 0x8009C090 | no |
  | func_8009C0A4 | 0x8009C0C0 | no |
  | func_8009C0D4 | 0x8009C0F0 | no |
  | func_8009C104 | 0x8009C120 | no |
  | func_8009C134 | 0x8009C150 | no |
  | func_8009C164 | 0x8009C180 | no |
  | func_8009C194 | 0x8009C1B0 | no |

  All 16 "army" sites came back negative under attract-demo probing — none of
  them widened m11 in this window. Given the array is stride-~0x30 and 16-wide
  (matches Wave Race 64's course count), the working hypothesis is these are
  **per-course camera setters never active during the attract loop**, not
  culling-only constants. Confirmed-negative-under-attract-demo is NOT the same
  as confirmed-non-projection — resolving them needs a real per-course race
  probe (drive automation into an actual race on each course), same caveat as
  the two round-1 inconclusive dispatch sites (0x8009BA40, 0x8009BB0C).
  Running tally: 4 confirmed projection movers (0x8009B93C, 0x8009BBC4,
  0x8009BCD0, 0x8009B1C8); 20 sites still need in-race testing to classify
  (2 dispatch + 1 standalone + 16 army + the ~25 remaining un-enumerated of
  the 49 total).

  **Round 3 (2026-07-16, scripts/classify_fov_sites_race.py + scripts/drive_to_race.ps1
  — real Sunny Beach race, not attract demo):** built input automation that
  drives the game from boot into an actual live Time Trials race on Sunny
  Beach (posts WM_KEYDOWN/UP straight to the game's HWND, no focus needed —
  see the script header for the exact confirmed menu sequence and the
  extended-key-bit bug that made every Down-arrow tap silently resolve to
  Numpad-2 instead). Re-probed the 19 sites round 1-2 could only mark
  "camera not active in attract window": the 2 round-1 inconclusive dispatch
  sites (0x8009BA40, 0x8009BB0C) plus all 17 remaining round-2 negatives
  (0x80089C50 + the 16-wide army 0x8009BED8..0x8009C1B0).

  **All 19 came back negative for the 66.5° projection, even under a real
  race.** Racing also surfaces m11 values attract mode never showed at all
  (0.577, 1.569, 2.148, 3.371, 4.688 — likely HUD/menu-overlay ortho
  projections and other non-fovy matrices caught by the same telemetry hook),
  but none of the 19 patched sites ever produced 1.527. Combined with the
  army's structural signature (stride ~0x30, each site an near-identical
  wrapper calling the same helper — func_8009BE40 for the dispatch-battery
  ones — with $a3=45.0 and two fixed pointer args), this is now good evidence
  these are genuinely **not projection/fovy movers**: most likely per-course
  or per-difficulty setters for some other camera parameter (angle/tilt,
  matching the earlier "two 45° fields" pitch-vs-fovy finding), or dead
  code paths for camera modes Sunny Beach + Time Trials + Normal difficulty
  don't reach. Final tally: **4 confirmed projection movers** total
  (0x8009B93C, 0x8009BBC4, 0x8009BCD0, 0x8009B1C8) out of the 24 sites
  actually tested across all three rounds; the remaining ~25 of 49 are still
  unenumerated (funcs_5/funcs_7 standalone-style sites beyond the three
  already found). Given rounds 2-3 agree on all 19 overlapping sites, further
  bisection of the army specifically is likely low-yield — if it's ever
  worth finishing, the next step is enumerating the rest of the 49 (not
  re-testing the army) and/or testing other courses/difficulties in case a
  handful of the army entries are genuinely per-course and Sunny Beach
  simply isn't the one that activates them.

  **47.75° instruction patches: VALIDATED (2026-07-15).** With all 49 inline-45.0f
  sites patched (scripts/gen_fov_patches.py generates the TOML block), RT64
  telemetry shows m11=2.255966 = 47.75° live — the recompiler patch mechanism
  provably moves the projection. User perception at +6% is (correctly) nil; one
  user screenshot (Dolphin Park) shows real scene geometry continuing into the
  margins. Remaining engineering for full border removal:
  1. ~~A 45° source still appears in telemetry alongside 2.256~~ **SOLVED
     (2026-07-16, round-4 group bisection via scripts/probe_fov_group.py +
     drive_to_race.ps1):** the in-race camera fovy is a 51st-site situation —
     it lives at **0x800A54E4 in func_800A52D8**, part of the 0x800Axxx block
     rounds 1-3 never enumerated. With only that site patched, a live race
     renders at 47.75° (m11=2.255966, no 2.414 left). func_800A52D8 has three
     sibling inline-45.0f sites (0x800A5648, 0x800A57AC, 0x800A57B8) — likely
     the other C-button camera view modes; untested, patch them if a view
     mode still renders at 45°. SHIPPED: 5 permanent [[patches.instruction]]
     entries in recomp/waverace64.toml (the 4 attract-demo movers + this).
  2. ~~The margin TINT~~ **SOLVED (2026-07-16):** the send_dl tint-widening
     rewrite already existed but widened the texrect to (0,0)-(319,239) —
     ONE PIXEL short of the widened scissor (0,0)-(320,240). RT64 only
     exempts a texrect from 2D aspect compensation (and lets it stretch
     across the full widescreen framebuffer) when rect.lrx >= scissor.lrx
     (`coversScissorWidth`, rt64_framebuffer_renderer.cpp ~1684); at 1px
     short it was pinned to the centered 4:3 and the expanded margins stayed
     untinted. Fixed by widening to exactly (320,240). Diagnostic that found
     it: WR64_TEXRECT_LOG=1 logs every texrect the DL walk sees.
  3. The wave-mesh detail region — still THE open blocker; see "Remaining
     empty margins" below for what was newly established and ruled out on
     2026-07-16.
  4. ~~Bisect the 49 sites~~ DONE across rounds 1-4 (see Site classification
     above): 5 confirmed projection movers shipped; the army is not
     projection; ~24 of the 0x800Axxx block remain individually unclassified
     but the only one that matters for the race view is 0x800A54E4.

  **Remaining empty margins (2026-07-16, after fovy+tint fixes — the current
  state of the hunt):** with the 5-site fovy patch + scissor + tint rewrites
  active, an ultrawide race still shows flat clear-color bands at the window
  edges (user-confirmed). What they are NOT (all tested on live Sunny Beach
  races via drive_to_race.ps1):
  - NOT the RSP viewport: theory was the world maps into the inner rect via
    the Vp transform. Falsified twice: no G_MOVEMEM/G_MV_VIEWPORT command
    appears at the TOP level of gameplay DLs at all (WR64_VP_LOG=1; they'd be
    in branched sub-DLs the linear walk skips), and an RDRAM scan for the
    inner-rect Vp signature — vscale=(604,396), vtrans=(636,476), plus
    relaxed either-half variants (poke scan mode 5, rescanning every 600
    updates well into the race) — found ZERO matching structs. The game does
    not keep an inner-rect viewport anywhere in RDRAM.
  - NOT the guFrustum array at 0x801D7B70: instrumented poke_frustum to dump
    active entries every 300 calls — the array stays ENTIRELY INACTIVE
    during real races too (stronger than the earlier attract-demo-only
    negative). WR64_POKE_FRUSTUM is a dead end for this; an apparent success
    in one run was race-circumstance (post-crash camera), not the poke.
  What they ARE (WR64_POKE_FOV=2.0 differential test, screenshots in
  drive_fovpoke/): with the frustum forced 2x wide, the SKY and TERRAIN
  coverage follow the camera and fill the top/upper margins — but the WATER
  mesh boundary does not move: same rectangle as at 47.75°. So sky/terrain
  coverage is frustum-fit (fixable by fovy alone, at the cost of a
  perceptible zoom-out — full vertical fill needs ~53.3°), while the wave
  grid is pinned to something else (screen-rect or its own bounds struct) —
  finding the wave-grid bounds source is the single remaining hunt for
  border-free ultrawide water.

  **Build-system trap (2026-07-16, root cause found):** builds appeared to
  "succeed" while linking stale objects — new code/log strings missing from
  the exe. NOT a cmake/ninja bug: (1) invoking `cmd /c scripts\rebuild.cmd`
  from Git Bash can silently do nothing — MSYS argument conversion mangles
  `/c` into a path, cmd prints its banner and exits 0 without running the
  batch (use PowerShell or Python subprocess, or `cmd //c` from bash);
  (2) an edit had also saved rebuild.cmd with LF-only line endings + UTF-8
  em-dashes, which cmd misparses silently — batch files must stay ASCII with
  CRLF. rebuild.cmd now documents both, calls ninja directly (twice; the
  N64Recomp-triggered CMake reconfigure makes the first pass conservative),
  echoes a named failure per step, and ends with a STALENESS GUARD: it
  exits nonzero listing any src/*.cpp|*.h newer than the linked exe, and
  prints "rebuild OK: <exe timestamp>" on success — a silent stale build is
  no longer possible.

  **Diagnostics added 2026-07-16 (all in src/rt64_render_context.cpp):**
  - WR64_TEXRECT_LOG=1 — log every texrect (decoded 10.2 coords) the DL walk
    sees; found the tint variant mismatch.
  - WR64_VP_LOG=1 — log G_MOVEMEM/G_MV_VIEWPORT commands (none at top level
    of gameplay DLs; kept for future sub-DL work).
  - WR64_POKE_SCAN now rescans every 600 updates (first at 300) — per-scene
    structs don't exist yet at update 300, which lands in the menus; the
    last scan before exit wins.
  - poke scan mode 5 — Vp-struct signature scan (exact + relaxed halves).
  - poke_frustum dumps all 9 fields of each active 0x801D7B70 entry every
    300 calls, including a tick line when nothing is active.

  **Endgame procedure (if pop-in is visible):** instruction-patch the 8 inline sites in small groups
  (via [[patches.instruction]] in waverace64.toml; 45.0f → ~47.7f needs
  lui 0x423E + ori pairing or lui 0x423F = 47.75 single-instruction) and classify
  each: moves projection (watch RT64-PROJ m11 telemetry), moves culling (buoys in
  margins in demo fb dumps), or tilts the camera (the pitch writer — exclude, or
  keep as a camera-angle enhancement knob). Then ship: fovy+culling sites at ~47.7°
  + wave-grid widen (still to find) + the scissor rewrite.
- **The detailed wave-mesh region does NOT follow** — its coverage is still sized
  to the inner rect (visible seam). The water grid has separate bounds. ✘
- fovy is written by inline 45.0f constants (`lui reg, 0x4234`) at:
  0x80089C50 (funcs_5), 0x8009B1C8 (funcs_7), and six sites in the 0x8009Bxxx
  dispatch-handler battery (funcs_8: 0x8009B93C, 0x8009BA40, 0x8009BB0C,
  0x8009BBC4, 0x8009BCD0, 0x8009BED8) — per-camera-mode setters that store into
  heap camera structs.

**Two 45° fields (2026-07-15, user-discovered):** at 3.0x FOV widening the real-race
view flips UPSIDE DOWN — the race camera struct contains at least two 45.0f fields:
the fovy AND a camera angle (pitch/elevation; 45°x3=135° tips past vertical). The
attract demos showed no change that run (per-scene camera coverage varies).
Isolation tooling added: every actively-re-read poke address is logged
(`[FOV] live poke at 0x...`), and `WR64_POKE_FOV_ONLY=addr[,addr]` restricts pokes
for one-variable-at-a-time runs. Current procedure: race at 3.0x → collect live
addresses → single-address runs → the zoomed-but-upright one is fovy; the flipping
one is the camera angle (a free camera-tilt knob for future enhancements).

**Remaining for shipping border removal:** (1) bisect the FOV candidates with the
visual margin test to isolate the live camera struct; (2) read the funcs_8 handler
code around one inline-45.0 site to learn the camera struct layout (fovy offset →
neighbors = the rest of the camera params); (3) find the wave-grid bounds (likely
computed from screen size or camera separately — the seam rectangle is the tell);
(4) proper patch: fovy 45° → ~47.7° (320/302 wider) + wave-grid widen + scissor
rewrite = seamless full-frame rendering.

**The frustum chain (earlier dead end):** `guFrustumF2` (0x801EE274) has exactly one
caller chain: `func_801EE46C` (guFrustum fixed-point wrapper) <- `func_800B4ABC`,
which iterates a static struct array at **0x801D7B70** (stride 0x24; entry active
when +0x00 != 0) and feeds guFrustum from fields +0x04 (int, left source, scaled
<<3 and negated), +0x1C (f32, right source, negated), +0x14 (f32, top source);
near/far/scale from +0x08/+0x0C/+0x10. `WR64_POKE_FRUSTUM=1` widens l/r/t by 1.3x.
**The array is INACTIVE during attract demos** (they use an overlay camera path) —
test interactively in a real race: if the view widens, this is the gameplay
projection source and likely feeds culling and the wave grid too.

- HUD elements stretch in widescreen (user-observed). The HUD is drawn in 2D ortho
  coordinates; keeping it 4:3 needs either RT64 extended-GBI tagging or game
  patches to reposition. Not started.

## HUD — SOLVED (2026-07-15), same fix as borders

Root cause of the widescreen HUD stretch: RT64 auto-compensates texrect (HUD)
aspect under Expand, but only when the framebuffer scissor ratio is within 10% of
the VI aspect (rt64_framebuffer_renderer.cpp ~1436, `SimilarityPercentage`). Wave
Race's odd game scissor (8,20)-(310,218) has ratio 1.525 vs source 1.333 — fails
the check — so RT64 disables compensation and the HUD stretches with the window.
With our scissor rewrite active (WR64_BORDERS=0 → scissor 0,0-320,240 = ratio
1.333) the check passes and the HUD keeps correct proportions. Verified by A/B
window captures at 1600x900 (hud_stretched.png vs hud_fixed.png — the WAVE RACE
logo oval). The border fix and the HUD fix are the same switch.

## Per-scene presentation — SOLVED (2026-07-16)

Goal: pure-2D menus (watercraft select) present as centered 4:3; everything
with a live 3D world (title flyby, demos, difficulty select over attract,
races) stays widescreen. Committed as superproject dcf7b42 + lib/rt64 2c1630c.

### Scene classification (what finally worked, and what didn't)

Signal = RSP-side counters in `lib/rt64` `rt64_rsp.cpp addCurrentProjection`
counting **segment-3 perspective projections that geometry actually draws
under**, split by camera:

- Live-world cams: races 45° (`m11=2.414215`), demo/flyby 75° (`1.301849`),
  transitions 50°-ish variants at phys `0x0012D8F0`.
- **The 2D menus render a parked 3D world every frame** (invisible on real
  hardware, revealed by Expand) under a **dedicated fovy=50° camera —
  `m11=2.148331`**, bit-stable guPerspective output, at its own matrix buffer
  (phys `0x001468D8`). That constant is the menu fingerprint.

Dead ends, for the record (each failed on real user runs):
1. Tint-texrect presence → flybys misclassified.
2. Texrect counts → difficulty select (many texrects, needs wide) is
   inseparable from watercraft select.
3. Linear DL scans for seg-3 `G_MTX` → false positives from **stale content in
   the double-buffered DL tail** (the walk ignores `G_DL` branch-variant
   termination and control flow generally) — even with branch termination,
   menus genuinely *load* the world matrix without drawing under it. Only
   "drawn under", counted inside the RSP, is reliable.

Smoothing: Schmitt trigger, ±15/frame with engage 60 / release 30 (~0.2 s),
moved ONLY by positive evidence (world draw vs menu-world draw); blank/fade
frames hold state so loading fades can't flip the aspect.

### Presentation (how 4:3 is applied crash-free)

**Never flip RT64 `UserConfiguration::AspectRatio` at runtime.** The aspect
drives render-target widths:
- `updateUserConfig(true)` (framebuffer discard) → destroyAll races the
  in-flight workload/present queues → crashes (Event Log: AV inside
  `VCRUNTIME140!memcpy`, plus jumps through corrupted pointers). Reference
  ports (Zelda64Recomp) only ever call this on explicit settings-menu clicks.
- `updateUserConfig(false)` → old wide targets survive → misaligned stale
  ghosting behind the menu.

Instead, rendering stays Expand permanently and the **final VI blit is
scissored to a centered 4:3 rect** (`rt64_wr64_set_present_crop43`,
`rt64_vi_renderer.cpp`); the swapchain is cleared before the blit, so the
pillars are true black. Instant, no resource churn, nothing to race.

Two rendering fixes make the cropped frame pixel-identical to a real 4:3
frame (`rt64_framebuffer_renderer.cpp`, both gated on the crop flag):
1. Force the fbPair aspect compensation on (`adjustRatio`) — fbPairs with
   odd scissor ratios otherwise skip it and content lands wide-spread.
2. Disable the wide-viewport heuristic: the menu draws each craft preview
   through a **framebuffer-spanning viewport scissored to its box**; the
   heuristic reads that as fullscreen world content and applies the
   widescreen spread. (This is also what the mysterious "floating jetskis in
   the menu margins" were.) The rider preview always took the non-wide
   squeezed path — which is why it was always positioned correctly.

### Verification tooling

`scripts/drive_to_menu.ps1` boots the game and navigates to the watercraft
menu with synthesized keyboard input (keybd_event **with real scancodes** —
SDL ignores VK-only events), capturing per-step screenshots + stderr.
`WR64_WINDOW=1920x800` reproduces ultrawide layouts on any screen. The final
fix was found and verified end-to-end this way, no manual testing.

## Emulator prior art

- GLideN64 "Crop" feature: crops in-framebuffer borders (Wave Race's type) but
  loses image area; their "Overscan" feature handles VI-placement borders. Our
  scissor rewrite recovers real image instead of cropping — strictly better for
  this game.
