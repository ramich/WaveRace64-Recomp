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
- **Stuttering clouds — SOLVED 2026-07-20** (the 2026-07-15 "unimplemented
  TODO / structural" conclusion was WRONG — see "Cloud stutter — SOLVED" below
  for the correction and fix; the short version: RT64's per-vertex velocity
  machinery is complete, it is merely gated off by the default TransformGroup,
  and our fork opens the gate with a vertex-count threshold).
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

## Wave-mesh "bounds" — SOLVED (2026-07-17): sub-DL scissors

The entire wave-grid-bounds mystery dissolved: the wave mesh was never sized
or culled to the inner rect at all — it is **scissored by G_SETSCISSOR
commands issued from branched sub-DLs**, which the port's top-level DL word
rewrite can never reach (it stops at the first G_DL). That's why the water
boundary was a crisp screen-space rectangle, why it never moved with fovy
(scissors don't), why no view-rect variables existed in RDRAM (bisect ruled
them out — the values are immediates inside whatever builds the sub-DL), and
why terrain (drawn under the top-level, rewritten scissor) filled the
margins while water didn't.

Fix (shipped): rt64 fork exports `rt64_wr64_set_scissor_widen[_mask]` — a
filter inside `RDP::setScissor` (rt64_rdp.cpp) that catches EVERY scissor
after segment resolution and widens inner-rect-signature ones (ul <=
(10,22), lr >= (300,214), 10.2 fixed) to (0,0)-(320,240). The port enables
it per frame for gameplay scenes only (menus keep original clipping), next
to the present-crop toggle in src/rt64_render_context.cpp.

**Selectivity is load-bearing:** 3 scissors match per race frame. Index 0
belongs to a far-ocean/horizon pass that MUST stay clipped — widening it
paints flat ocean over the terrain margins (the first widen-everything
attempt regressed exactly that way). Indices 1-2 are the wave-mesh passes.
`WR64_SCISSOR_MASK` (hex, default 0x6) picks which indices widen; re-bisect
per course if other tracks order their passes differently.

Result on Sunny Beach ultrawide (1920x800): sky and wave water reach much
further toward the window edges. Remaining artifacts below.

### Scissor-widening addendum (2026-07-17): what each layer does and breaks

Hard-won map of the interacting layers (each verified by pixel-measuring
zoomed screenshots — do NOT trust full-window thumbnails, the cyan bands
blend into water):

- RDP-state widening of scissor index 0 **must stay OFF** (mask default
  0x6). Index 0's rect drives RT64's fbPair/projection aspect
  classification; widening it collapses the whole world into an unstretched
  centered 4:3 band (user-reported regression; "the gameplay rectangle
  stays 4:3 and doesn't stretch"). The earlier "far-ocean overpaint" theory
  was this same collapse, misread.
- The per-CALL clipping that index 0 causes is instead neutralized at
  GPU-scissor conversion time (rt64_framebuffer_renderer.cpp, gated by
  `rt64_wr64_get_wide_world()`), where placement/aspect decisions are
  already made. WR64_CLIP_DEBUG=1 dumps per-call clip inputs.
- `rt64_wr64_set_wide_world` also forces scissor-covering gameplay
  perspective projections onto the wide-viewport path so the game's
  camera-bob viewport translation can't knock them off it mid-race.
- The game's world viewports are ALREADY full-size (scale 160x120; verified
  via the rt64-side setViewport trace WR64_VP_TRACE=1) — the old
  "inner-rect RSP viewport" theory is dead; the bob is a translate.

**Remaining artifact — the last boss (unsolved): the game CPU-clips its
large world polygons (beach strip, banner cloth, shore) to its view
rectangle** before building the DL. Evidence: with every GPU scissor
verified full-frame (WR64_CLIP_DEBUG shows no inner-rect call scissors
anywhere), banner/fence/beach still cut in a perfect vertical at fb x~=9 —
the old view-rect edge, unmoved by the fovy patch. Falsified sources so
far: all 15 in-race s16 view-rect pairs in RDRAM (poked, no effect), code
immediates 310/218/302/198 (only the tint builder uses them), float
immediates 310.0f/218.0f (zero hits). The clip constants live in some form
not yet identified — likely inside the polygon-clip routine as derived or
shifted values.

**Shore/banner hunt, round 2 (2026-07-17 pm — negatives + system map):**
- View-rect 4-TUPLES don't exist in RDRAM either (new poke scan mode 6:
  s16[4]/s32[4]/f32[4], both orderings, all rect flavors — zero hits
  in-race). The bounds are definitively code-derived.
- WR64_CLIP_DEBUG=2 (log EVERY call whose scissor ulx==32): ZERO hits in a
  full race — confirms no RT64-visible draw call is scissored to the inner
  rect; the geometry itself ends at fb x~=9. Under widen-all the same
  endpoint maps to the squeezed band edge (~460px) — same fb-space limit,
  different mapping, so it is content generation, not GPU clipping.
- Frame composition mapped (decomp func_8009328C, 1P):
  func_8008FB74 (waves) -> func_8006E674 -> func_800687A4 -> func_8007FFA8
  -> func_800ADF90 (course OBJECTS) -> Draw_WaterEffects -> func_80069594
  -> func_80068538 -> configSignalRectangle -> func_800B305C ->
  func_8008BD2C. The banner/fence/beach-people are drawn by one of these;
  buoys/objects by func_800ADF90.
- **Object system found (decomp code_52CD0.c):** func_800ADF90 iterates 30
  object slots with per-type draw dispatch; func_800ADE14 is the
  visibility test — an ANGLE cull `fabs(normalize(angleToObj - camYaw)) <
  100.0f`, threshold constant at **0x800ADF54** (`lui $at, 0x42C8`),
  instruction-patchable. A 160.0 test build raced fine but the A/B was
  inconclusive (start-line screenshots landed in the race-entry fade) —
  redo with a proper capture; note 100 units are probably NOT degrees of
  half-FOV, so this may only matter for behind-camera pop.
- The old "0x43C1=386.0 focal" lead (386.27 = 160/tan(22.5)!) at
  0x80082A68 in func_80081CC8 sits in a 236.0 < v < 386.0 range check;
  widening both constants (100/500) had NO visual effect on the shore cut —
  not the clamp (or not the relevant instance).
- NEXT: identify which composition pass draws banner/fence/beach (RT64 F1
  inspector draw-call debugger is the right tool — WR64_DEV=1, click the
  geometry; or bisect passes by stubbing func_8006E674 / func_800687A4 /
  func_8007FFA8 one at a time via instruction patches jr-ra), then read
  that builder for its generation bounds, wave-grid style.

**Pass identification by stubbing (2026-07-17 pm, jr-ra instruction patches
+ one race each; drive_stub_*/ dirs):**

| pass (1P chain order) | stub result -> role |
|---|---|
| func_8008FB74 | wave mesh (known) |
| func_8006E674 | **course world**: menus hang at course select with the preview map's center black — draws terrain/shore/banner/fence/people AND the course preview. Race can't start without it |
| func_800687A4 | rider/jet-ski GONE -> player renderer |
| func_8007FFA8 | no obvious change in race frame (post-effects?) |
| func_800ADF90 | course OBJECTS (ramps/signs; angle cull func_800ADE14 @100.0f, const at 0x800ADF54) |
| func_80069594 | no obvious change |
| func_80068538 | **R buoy GONE -> the buoy renderer** (buoys are NOT func_800ADF90 objects; their pop-in cull lives in here — unexplored) |
| func_800B305C / func_8008BD2C | subtle marker/prim-color changes only |

**THE ARCHITECTURAL ANSWER — 18-sector precomputed visibility (PVS):**
inside func_8006E674 (funcs_3.c ~line 5655 / vram 0x8006F0D0-0x8006F19C):
`sector = clamp(trunc(viewAngle / 360 * 18), 0..17)` — the camera yaw picks
one of **18 x 20-degree sectors**, then a mirrored branch (sector >= 10 ->
sector = 18 - sector) selects a set of segment-1 COURSE DATA pointers
(0x0102CC58/CC70/CD90/CD78 vs 0x0102CCE8/CD00/CE20 on Sunny Beach) — i.e.
**per-view-angle prebaked DL/vertex sets**. The world content for each view
direction was authored/cooked offline for the original 45-deg 4:3 view.
There IS no clip constant to widen: geometry beyond the old view edge
simply is not in the selected sector's data — it lives in the NEIGHBOR
sector's set. This explains every stubborn symptom: fovy-independent
culling, no rect values anywhere in RDRAM or code, the crisp screen-space
cut (sector content ends where the next sector's begins), buoys popping
(their renderer likely does its own angle test against the same sector
math or the 100-deg object cull).
FIX DIRECTIONS: (a) emit the current sector's AND a neighbor's DL sets
(patch the selection at 0x8006F13C/0x8006F16C to also draw adjacent sector
data — overdraw/z risk, needs experiment); (b) buoys separately: find the
angle cull inside func_80068538 (fresh, likely a single constant like the
object cull's 100.0f); (c) accept as structural like the foam.

## Wave-grid coverage — SOLVED (2026-07-17, via the decomp): grid dims at 0x800DA8B4

With the decomp cloned (github.com/ramich/Wave-Race-64), the water pipeline
fell out fast: `func_8008FB74` (src/game/code_43DA0.c area, chained into
`Draw_WaterEffects`) is the wave-mesh DL builder, and it re-reads a static
config block at **0x800DA8B4 = {flag=1, rows=19, cols=35, then fullscreen
Vp structs (the later ones are the splitscreen viewport variants)}** every
frame. The detail-water mesh is a rows x cols camera-facing grid — 19x35
covers exactly the original 4:3 view, which was the visible "detail water
rectangle" (user-screenshotted at the race start). Runtime-poking the dims
(new generic tools: `WR64_PEEK=addr[,addr]`, `WR64_POKE_WORDS=addr:val[,..]`)
enlarges the grid live: 21x47, 23x55, 27x63 all verified stable at full
frame rate with detailed foam water spreading accordingly. SHIPPED: the
port writes rows/cols each gameplay frame (default 23x55, override
`WR64_WAVEGRID=RxC`, clamped 40x96) in src/rt64_render_context.cpp.

Still open after this: the terrain/shore CPU clip above (beach strip,
banner still cut at the old view-rect edge — conventional course geometry,
no sibling config block found near 0x800DA8B4); water foam/sparkle
fb-effect only covers the original fb region (RT64 structural); a thin
bottom water strip at extreme angles (grid extent, mostly camera-dependent
now).

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

## Input / controls architecture — SOLVED (2026-07-18)

Symptom: rebinding a control in the RecompFrontend launcher's Controls tab had
no effect in-game (e.g. remap A→Space, but A still fired on X and Space did
nothing). Root cause: the port's `src/input.cpp` read SDL directly
(`SDL_GetKeyboardState` + a hardcoded `SDL_SCANCODE_*` → N64-bit table, and
direct `SDL_GameController*` polling), completely bypassing recompinput. The
Controls tab was writing bindings nobody read.

Fix (all under `#ifdef HAS_RECOMPUI`, with the old direct-SDL path kept as the
non-launcher fallback):
- `input_get` now calls `recompinput::profiles::get_n64_input(0, buttons, x, y)`
  — it aggregates the active keyboard + controller profiles, honours the user's
  remapped bindings, applies the joystick deadzone, and suppresses game input
  while a menu is capturing input.
- `input_poll` now drives `recompinput::poll_inputs()` + `update_rumble()` each
  frame; controllers are discovered by recompinput's SDL event filter (via
  `recompinput::handle_events()` on the gfx thread), not by the port.
- `main.cpp` calls `recompinput::players::set_single_player_mode(true)` after
  `config::finalize()`. This is REQUIRED: `single_player_mode` defaults to false,
  and `get_n64_input`'s multiplayer branch needs player-assignment; single-player
  mode makes it use the SP keyboard/controller profiles directly. Those SP
  profiles are created during `finalize()` → `load_controls_config`.

Key recompinput facts (fork under ramich/RecompFrontend): binding edits and the
read path share one array (`input_profiles[i].mappings`), so edits apply live;
`should_override_keystate` only suppresses Alt+Enter; bindings persist in
`controls.json`.

## Border toggle = boot-time aspect — SOLVED (2026-07-18)

The launcher "Show Borders" toggle must map to RT64's aspect ratio, chosen ONCE
at renderer construction and never flipped at runtime (a runtime
UserConfiguration aspect change crashes in-flight queues with discardFBs, or
ghosts stale targets without — same constraint as the per-scene present-crop
design). Mapping:
- Border removal (default) → `AspectRatio::Expand` (image widened to the window;
  menus pillarboxed by the VI-blit crop).
- Show Borders (stock) → `AspectRatio::Original` (native 4:3 with the game's own
  black borders, correct proportions, pillarboxed). All widescreen widening
  hooks go inert.

So the setting is **restart-required** (labelled as such in the UI).

TRAP (the real bug behind "borders don't work even after restart"):
`RT64Context::update_config` fires on the launcher's startup graphics-config
apply and was unconditionally forcing `Expand`, silently reverting the
constructor's `Original`. Fix: `update_config` must mirror the constructor —
`Original` when `s_show_borders`, else `Expand`. Earlier dead ends: forcing the
present-crop on for all borders-on scenes (squished gameplay — the crop is
menu-only in BOTH modes); the live-toggle crop-latch left stale wide content in
the margins (sky/water instead of black) because Expand rendering persisted.
Lesson: with a fixed-at-boot aspect, don't try to make it look right live —
make it correct on restart and say so.

## Frame interpolation & the cloud stutter — INVESTIGATED (2026-07-18); SOLVED 2026-07-20 (see the SOLVED section below)

Goal: fix the `WR64_HIGHFPS=1` cloud stutter. Two parallel investigations
(RT64 internals + decomp) established:

RT64 side (`lib/rt64/src/hle/rt64_game_frame.cpp`, active `GameFrame::match` at
~:255 — NOT the commented reference block that ends ~:1041):
- Interpolates four channels: view/proj transforms, world transforms (RigidBody
  linear+angular), RDP **tiles** (UV/tile-descriptor scroll), and lookAt.
- Per-vertex **velocity interpolation is wired in the shaders**
  (`RSPWorldCS.hlsl`) **but never fed** for this game — the velocity buffer is
  always zero. It's gated behind `vertexInterpolation != G_EX_COMPONENT_SKIP`,
  set only by extended-GBI commands WR64 never emits. So a mesh with a static
  transform but regenerated vertices is **held static across the interpolated
  sub-frames, then snapped** = the stutter.
- **Tile/UV interpolation is on by default** and would smooth continuous texture
  scroll — BUT it rejects per-frame deltas that look like a wrap/page-flip
  (≥ mask×2). A texture-segment swap therefore won't interpolate.
- Draw calls are matched by a render-state hash + nearest-transform best-fit
  (WR64 emits no extended-GBI IDs).

Decomp side (`C:\dev\src\github\Wave-Race-64`): the in-race sky/clouds are 3D
frustum geometry drawn inside **`func_8006E674` (0x8006E674), which is NOT
decompiled** (GLOBAL_ASM stub), so the exact cloud motion can't be read from C.
The engine's three animation techniques: (a) per-frame vertex regen (the wave
mesh `func_8008FB74`), (b) per-frame `gSPSegment` texture-bank swaps
(`func_80091DBC`) = page-flips, (c) matrix billboarding (`func_8006CB98` et al.)
— note (c) would already interpolate, so the old "billboards regenerate
vertices" note is partly self-contradictory. Best-supported causes: (a) vertex
regen or (b) texture-segment page-flip.

Decisive test not yet done: read the live cloud DL frame-to-frame (F1 inspector
or a targeted `send_dl` logger) to see whether the Vtx buffer, the tile/segment,
or only the matrix changes. NOTE: the F1 inspector in this fork is minimal —
hovering/clicking geometry does nothing — so a `send_dl` diagnostic logger is
the realistic route. Fix paths by outcome: (a) → game-patch the cloud DL builder
to emit motion, or scoped vertex interp (hard: vertex correspondence for a
count-unstable mesh); (b) → hold cleanly / retime, page-flips can't be tweened;
(c) → matching bug, should already work.

## EEPROM save format (Eep4k) — for save editing

WR64 registers `SaveType::Eep4k` (4 kbit = 512 bytes). The runtime persists it
to `saves/waverace64.bin` (+ `.bak`) under the config path (the exe's working
dir for this build). Decomp: `src/game/core/wr64_save.c`.
- Layout (partially typed — most of the 512-byte struct is untyped `pad`):
  `0x00` s16 magic (`"TE"` = 0x5445), `0x02` u16 checksum, `0x04..0x1FF` payload
  (player names, per-course records/ghost times, and the course-unlock/
  championship-completion flags).
- **Checksum** (`Save_GenCheckSum`): 16-bit sum of bytes `[4..511]`, stored big-
  endian at `0x02`. Verified against a live save (0xc863). Trivial to regenerate
  after an edit.
- **Course unlock — SOLVED 2026-07-18 via the .bt template.** Time-Trial course
  availability is derived from **difficulty completion**, not per-course lock
  flags. Completion fields (offsets verified byte-for-byte against a live save):
  - `0x08` normal (1 default → 6 finished), `0x09` hard (0 → 6),
    `0x0A` expert (0 → 7), `0x0B` reverse (0 → 7)
  - `0x0C` completion bitfield. The four `didFinish` bits are the **LOW nibble →
    `0x0F`**. CORRECTION: my first attempt used `0xF0` (wrong nibble); it unlocked
    the courses but **muted the menu music** — that mute was the tell that the
    packing was reversed. Verified: `0x0F` unlocks AND keeps music.
  - To unlock all: write `0x08..0x0C = 06 06 07 07 0F`, then regenerate the
    checksum (sum bytes[4..511] & 0xFFFF at 0x02).
  Shipped as launcher **Enhancements → Unlock All Courses**
  (`wr64_unlock_all_courses()` in src/main.cpp: edits
  `get_config_path()/saves/waverace64.bin`, writes a `.unlock_backup`, applies on
  next game start). NOTE: the `unk50[3][3]` array at `0x50` (`{00 05 03}×3`) is
  the wave/race **conditions**, NOT unlock. External reference:
  KilianSteenman/N64-Save-file-formats `wave-race-64.bt` (010 Editor template).

## Later-course crash — forensics pending (2026-07-18)

A reproducible crash in a later course. Windows Event Log (Id 1000): exception
`0xc0000005` (access violation) immediately followed by `0xc000041d` (fatal
exception in a callback), **faulting module "unknown"**, fault address a full
64-bit VA that **varies between runs** (ASLR-shifted). Signature = execution
jumped to a computed/garbage address (bad indirect call/return target), not a
fixed function — consistent with either a missing indirect-call symbol split or a
game-logic bad pointer hit only on that course. The Event Log can't name the
function (module "unknown"); a minidump/stack-trace crash handler
(`SetUnhandledExceptionFilter`) would capture the recompiled caller from the
stack. Not a full-machine save-state candidate — recomps run native across real
threads, so emulator-style save states aren't feasible (RDRAM+context snapshot at
a frame boundary is a research project, not a quick add).

Minidump handler shipped and DID catch a real crash (2026-07-23), but the
first catch was useless: the dump resolved to a raw
`WaveRace64Recomp+0x3ee9df` offset with a corrupted stack unwind (the crash
profile — jump to a garbage address — makes stack-walking unreliable
without symbols to anchor it). **The build was Release with no debug
info at all** — `/Z7` only touched the MIPS-patches compile, not the main
port sources, and there was no `/DEBUG` linker flag, so no PDB existed to
resolve anything, ever. Fixed: `target_compile_options(... /Z7)` +
`target_link_options(... /DEBUG /MAP:...)` on the main `WaveRace64Recomp`
target (CMakeLists.txt, MSVC-only block) — emits a full PDB (covers every
statically-linked static lib too, including the recompiled game functions)
and a linker `.map` as a human-readable fallback. Zero runtime cost (debug
info doesn't touch codegen), PDB/map are ~9MB/~17MB and intentionally not
copied into `dist/`. Next crash's dump should resolve to actual function
names.

## HD texture replacement (RT64 packs) — wired 2026-07-18

RT64 has a first-class texture-replacement system (TextureCache + replacementMap,
F4 toggles `textureMap.replacementMapEnabled`). It was inert in the port because
`useConfigurationFile=false` left no data dir and no pack was ever loaded. Now
driven by the launcher **Textures** tab + `WR64_TEXPACK`/`WR64_TEXDUMP`.

- **Load a pack:** `textureCache->loadReplacementDirectory(ReplacementDirectory(dir))`
  + `replacementMapEnabled=true`. A pack dir = `rt64.json` (ReplacementDatabase)
  + image files. DB top-level keys: `configuration`, `textures`, `operationFilters`,
  `shiftFilters`, `extraFiles`. `configuration` = {configurationVersion:3,
  autoPath:"rt64"|"rice", defaultOperation:"stream", defaultShift:"half",
  hashVersion:5}. A texture entry = {path, hashes:{rt64,rice}, operation, shift}.
  `autoPath:"rt64"` auto-matches files named `<rt64hash>.png` (16-hex, lowercase,
  from `ReplacementDatabase::hashToString(uint64_t)` = `%016llx`). `autoPath:"rice"`
  parses `<rom>#<ricekey>#..._all.png` filenames (ricekey = text between the first
  `#` and last `_`, e.g. `1279903a#0#3`).
- **Dump:** set `state->dumpingTexturesDirectory` (TextureManager::dumpTexture,
  rt64_rdp_tmem.cpp). Writes per texture, hash-named: `.tmem`, `.tile.json`,
  `.rice.rdram`, `.rice.json` (+ `.rice.palette.*` for CI). RAW N64 data, not PNG.
- **Decode:** `scripts/decode_texture_dump.py` — decodes the linear `.rice.rdram`
  (+ palette) using `.tile.json` dims/fmt into PNGs, contact sheets, and a
  loadable `rt64.json` pack. CRITICAL: this runtime stores RDRAM byte-swapped
  within 32-bit words (the `^2`/`^3` addressing), so `.rice.rdram` must be
  32-bit-word byte-reversed before decoding (else pure noise). Formats seen:
  CI8, RGBA16, RGBA32, IA8, IA16, I8.
- **Port reconcile (thread-safety):** launcher setters (UI thread) only stage
  statics + a dirty flag; `apply_texture_state_gfx()` (called from update_screen,
  gfx thread) does the actual load/enable/dump so it never races send_dl. The
  Enable checkbox tracks F4 via update_option_value+apply_option_value; the title
  shows the live state.
- **Rice packs — SOLVED 2026-07-18 (no engine change).** Community hi-res packs
  use Rice naming (`<rom>#<crc>#<fmt>#<siz>_all.png`, often nested). RT64 matches
  by its OWN hash at runtime and does NOT compute Rice CRCs live, BUT its loader
  already resolves `autoPath:"rice"` databases: for each `textures[]` entry it
  finds the file whose parsed rice key == `hashes.rice` and registers it under
  `hashes.rt64` (resolvePaths, rt64_replacement_database.cpp; FileSystemDirectory
  recurses subfolders). So the fix is a DATABASE, not runtime code:
  `scripts/decode_texture_dump.py <dump> --rice <packdir>` computes each dumped
  texture's rice key (RiceCRC32 ported from RT64's own tools/texture_hasher —
  validated 23/24 against a real pack; reads .rice.rdram as native LE u32, no
  byte-swap for 32-bit reads; CI adds `#<paletteCRC>` from CalculateMaxCI*) and
  writes an `autoPath:rice` rt64.json into the pack folder mapping rice->rt64.
  Coverage = what's in the dump (a texture maps only if it was dumped); dump more
  screens/courses and re-run to grow it. Only the hash-index json is written; pack
  images are referenced in place. RT64's `tools/texture_hasher` (Rice mode) is the
  upstream equivalent if you'd rather build it.

## Distributable releases (how the ROM-less binary works) — 2026-07-19

Studied BanjoRecomp v1.0.1 to answer "how do they ship a compiled release?"
(clone at `C:\dev\src\github\BanjoRecomp`). The whole N64Recomp family
(Zelda64Recomp, BanjoRecomp, us) uses the same model:

- **The binary contains recompiled *code*, never game *assets*.** N64Recomp
  translates the MIPS code → C at build time; that C compiles into the exe. The
  generated C (`RecompiledFuncs/`) is `.gitignore`d — regenerated each build,
  never committed. Assets (textures/audio/models/course data/text) are never
  extracted; they stay in the ROM.
- **Assets are read from the user's own ROM at runtime.** The player supplies a
  legally-owned ROM through the launcher (`add_start_game_or_load_rom_option()`,
  RecompFrontend — WR64 already wires this at main.cpp). It validates the SHA-1,
  stores the ROM, and loads assets from it. A downloaded exe with no ROM prompts
  on first launch.
- **Building still needs the ROM** — the "no ROM to build" claim is a
  misconception. BanjoRecomp's BUILDING.md requires a decompressed ROM to run
  N64Recomp. Their CI gets it via the **private-repo trick**: every job in
  `.github/workflows/validate.yml` has a `Get extra dependencies` step that
  `actions/checkout`s a *private* repo (`secrets.SECRET_NAME` + `SECRET_TOKEN`)
  into `extra/`, then `cp extra/* .` before running the recompiler. The ROM lives
  only in that private repo + transiently on the runner — never in the public
  repo or the release artifact. Their syms are a public submodule
  (`BanjoRecompSyms`); ours (`recomp/waverace64.us.rev1.syms.toml`) are committed
  too, so the ROM is the ONLY private input we'd need.
- **Release archive contents:** exe + runtime DLLs (SDL2/dxcompiler/dxil + MSVC
  runtime on Windows) + the port's *own* `assets/` (launcher art/fonts/controller
  db). No ROM, no game assets. Our `dist/` layout already matches this.
- **Legal posture** (theirs and ours, not legal advice): recompiled code treated
  as transformative, assets never shipped, user must own the ROM. Untested in
  court. Documented in README "Releases & Distribution" + "Legal".
- **NEXT (Phase 8):** a GitHub Actions workflow mirroring Banjo's — build
  N64Recomp/RSPRecomp, pull ROM from a private repo via secrets, recompile,
  build with portable LLVM, package the `dist/` file set, attach to a tag.

## Windows app-icon embedding trap — 2026-07-19

Regenerating `resources/wr64.ico` alone did NOT change the icon embedded in the
exe, even though the exe relinked. Cause: `llvm-rc`'s dependency scan doesn't
look *inside* `wr64.rc` to see the `ICON "wr64.ico"` it references, so CMake/ninja
only recompile the resource when the `.rc` *source* changes — not when the `.ico`
changes. The exe kept relinking against a stale `wr64.rc.res`. Fix (CMakeLists,
WIN32 block): `set_source_files_properties(resources/wr64.rc PROPERTIES
OBJECT_DEPENDS ".../resources/wr64.ico")` so the icon is always re-embedded when
it changes. To force once: delete `build/CMakeFiles/WaveRace64Recomp.dir/
resources/wr64.rc.res` and rebuild. Also note Windows caches exe icons in
Explorer/taskbar — `ie4uinit.exe -show` (or clearing `IconCache.db`) to refresh
the shell even after the exe is correct.

## 2P VS split-screen widescreen — SOLVED (2026-07-20), two known-issues remain

The widescreen/border work was all tuned for 1P (one full-screen viewport). 2P
VS split-screen renders as TWO full-width, HALF-height, border-inset perspective
viewports (top scissor `(32,48,1244,480)`, bottom `(32,488,1244,916)`, race cam
m11=1.043900 ~87deg; fbPair scissor is the FULL frame `(0,0,1280,960)`). The
scene classifier already handled 2P correctly (race->wide, menu->menu); the
fixes were all in the rt64 render layer. Commits: rt64 1e023e1 + 518f5b1;
superproject bbc27f9 + 7a1ca8d.

- **Fills the width (rt64_framebuffer_renderer.cpp):** the stock wide-viewport
  test requires the projection to cover the whole fbPair width, which the
  border-inset halves fail, so they never widened (race stuck 4:3, flickering
  margins). Added `wr64SplitHalf` detection — near-full-width (within pairW/12)
  AND height in a tight band around HALF the frame (38-55%) AND flush-top or
  starting ~mid — and put it on the wide path. Full-height 1P views never match.
- **Correct proportions (rt64_projection_processor.cpp):** the inset half also
  failed the stock coversWholeWidth gate that drives the horizontal-FOV widening,
  so its 4:3 world was STRETCHED to fill the wide half (fat riders). Give the
  split half the SAME widening a 1P full-frame view gets (`1/aspectRatioScale`).
  CRITICAL: an EXTRA projH/pairH factor (tried first, "widen more because the
  half is 2x wider aspect") OVER-widens (thin/stretched riders) — the game
  already compensates its split-screen vertical FOV, so no extra. Verified by an
  empirical WR64_SPLIT_FOV sweep. Crop-gated (never touches menus).
- **Gutter garbage + divider seam (rt64_vi_renderer.cpp):** shared full-frame
  passes (start-gate arch, countdown lights, water) spill into the border bands
  ABOVE the top half, BETWEEN the halves, and BELOW the bottom half. Fix: the VI
  present blits ONLY the two play bands as separate scissored draws, leaving
  top/mid/bottom as the pre-cleared black swap chain. CRITICAL: must be
  PERSISTENT state (port sets it each game frame from a split-half draw counter,
  clears otherwise), NOT one-shot — the game runs 20 Hz but presents higher with
  interpolation, and a one-shot reset-per-present left interpolated frames
  ungutted => flicker. Default single full band = ordinary present (1P intact).
- **Craft-select edge leak (rt64_vi_renderer.cpp):** parked-world craft sit at
  the extreme 4:3 frame edge (overscan margin) in the 2P watercraft-select; our
  exact 4:3 crop exposes them (aspect-dependent as the crop edge lands sub-pixel
  in/out). Fix: ~8% overscan zoom on cropped-menu present (enlarge the blit about
  its center). Menu panels live inside the overscan-safe area, nothing clipped.
- **Retire crashes:** retiring from a 2P race chained "Failed to find function"
  at 0x8009A764 then 0x8009A818 — indirect-call targets the JAL scan missed.
  Split both (find_indirect_targets.py --split). More may surface on other exits.

### 2P automation (scripts/drive_to_2p.ps1, scripts/log_keys.ps1)
CRITICAL launcher/input facts (else the driver silently lands in the wrong
mode): the RecompFrontend launcher is up FIRST — ONE Enter starts the game (first
item pre-highlighted; behaviour has varied, watch for it). The active control
map is A=Space, stick=WASD (S=down); X is unbound. MUST park the mouse in a
corner (SetCursorPos 0,0) — RecompFrontend highlights on HOVER, so a cursor over
the window overrides the keyboard highlight and Enter selects the wrong item.
Menu order CHAMPIONSHIP/TIME TRIALS/STUNT MODE/2P VS/OPTIONS (2P VS = Down x3).
Logs are plain ASCII (grep -a). WR64_FBP_DEBUG=1 dumps per-projection layout.

### Flyby lower-half ghosting — SOLVED (2026-07-20): scene cross-matching
During the fast pre-race intro pan the BOTTOM half ghosted (motion-only,
invisible in stills — user-verified live). Root cause found by instrumenting
rt64's scene matching (WR64_MATCH_DEBUG=1, TEMP logging in
rt64_game_frame.cpp matchScenes): frame interpolation pairs current scenes to
previous scenes greedily by camera-matrix similarity — and during the flyby the
two halves' cameras traverse the SAME path offset in time, so the trailing
camera's current matrix is genuinely CLOSER to the leading camera's PREVIOUS
matrix (measured cross-pair diff ~130-270) than to its own previous (~313).
Greedy latched the wrong pairs ((2<-0)/(0<-2) sustained across the flyby) and
the displaced scene interpolated between wildly different cameras (diff
540-780) => ghost. No similarity bias can fix this — the wrong pair really is
closer. Fix (rt64 fork rt64_game_frame.cpp): when cur/prev scene COUNTS are
equal, pair scenes BY ORDER (WR64 builds scenes in a fixed draw order every
frame); greedy similarity remains as the fallback for count transitions.
Verified: 1983 by-order pairings, zero cross-pairs, and the user confirmed the
flyby is clean by eye. 1P unaffected (single world scene pairs 0<->0 as
before). A first attempt (1.25x penalty on out-of-order pairs) was
insufficient — kept in the greedy fallback as a mild bias. WR64_MATCH_DEBUG=1
stays available for future interpolation hunts.

### KNOWN-ISSUES
(none currently for 2P split-screen)

### NOT a bug: 2P engine audio absent — ORIGINAL GAME BEHAVIOR (closed 2026-07-20)
The player-engine voice is silent in 2P VS while music/other audio play
normally. Initially suspected as a port voice-allocation bug (1P vs 2P dump RMS
nearly identical, 3960 vs 4114 — music masks the missing engine; no RSP
"unhandled jump target" errors in 2P, so the recompiled microcode path is
clean). CLOSED: the user reproduced the exact same behavior in an emulator —
the game itself drops the engine SFX in 2P split-screen (a typical N64
voice-count/CPU budget cut). As designed; do not chase.

## Menu craft-preview aspect — SOLVED (2026-07-20): the double-squeeze

In widescreen (Expand + menu crop), the watercraft-select's four upper preview
craft rendered too NARROW (looked "vertically stretched"). Measured against
stock (WR64_BORDERS=1): craft bbox aspect 1.00 vs 1.79, height identical, width
0.560x — exactly 1/1.8 = one extra aspect factor at a 1920x800 window. Cause:
those craft draw under a perspective projection whose scissor covers the whole
menu fbPair, so rt64's projection processor FOV-widened its matrix (x *= 1/ars)
— and the crop43 design forces everything onto the squeezed path, which applies
screenScale.x = 1/ars AGAIN. Net (1/ars)^2: one squeeze correct, one distortion.
The RIDER preview was always right because its projection doesn't cover the
pair width (no FOV adjust -> single squeeze). This was introduced by the crop43
design (dcf7b42) as the lesser evil vs the old wide-path "floating jetskis in
the margins". Fix (rt64 cd88585, rt64_projection_processor.cpp): while
present-crop is active, skip the FOV adjustment for perspective projections —
the single squeeze is then the correct 4:3 placement. Verified by measurement
(normalized craft aspect 1.019 vs stock 1.000) and visually; gameplay and wide
menus are crop-off, unaffected. Method note: scripts/drive_to_2p.ps1 now
respects a caller-set WR64_BORDERS=1 for stock-mode ground-truth captures, and
the measurement lives in the session scratchpad (measure_craft.py pattern:
segment the black preview boxes, bbox the colored craft, normalize by box).

## Cloud stutter — SOLVED (2026-07-20): the velocity gate, and a vertex-count threshold

The high-FPS cloud stutter is fixed (user-verified: clouds smooth, water
unchanged). Two prior conclusions corrected:

- WRONG (2026-07-15): "vertex-level velocity interpolation is an unimplemented
  TODO". That TODO is in the COMMENTED-OUT reference block of
  rt64_game_frame.cpp. The ACTIVE matchTransform (~:790) computes per-vertex
  velocities generically: matched transform + equal vertex counts + changed
  position hash => velFloats = cur - prev, uploaded and consumed by the
  shaders. No extended GBI required for the mechanism itself.
- The real gate: the default `TransformGroup` has `vertexInterpolation =
  G_EX_COMPONENT_SKIP` (rt64_transform_group.h) while every other component
  defaults AUTO. Games without the extended GBI use exactly ONE transform
  group — the default pushed at workload reset (rt64_workload.cpp) — so their
  regenerated-vertex meshes never interpolate. That is the precise meaning of
  the earlier "wired but unfed".

Fix (rt64 8dbccbf): fork hook `rt64_wr64_set_vertex_interp(maxVerts)` — the
workload's default group gets `vertexInterpolation = INTERPOLATE`, and
rt64_game_frame.cpp skips velocity for transforms with more than `maxVerts`
vertices. The threshold is essential, discovered the hard way:

- First attempt interpolated EVERYTHING with changing vertices. Clouds fixed —
  but the WATER broke (user-caught): the wave grid is CAMERA-ANCHORED (it
  follows the craft), so vertex (r,c) is a grid slot, not a persistent world
  point; interpolating blends wave heights of different world positions and
  the animation warps/stutters.
- Telemetry (`WR64_VTXINTERP_DEBUG=1`, logs per-transform vertex count + max
  delta) showed a clean split: wave-mesh chunks are 350-870 verts per
  transform; the drifting cloud/sprite quads are 4-14 (48 max). Threshold 64
  separates them exactly.

Port default: ON with threshold 64 (`WR64_VTXINTERP`: 0 = off, 1/unset = 64,
other N = threshold N; rt64_render_context.cpp ctor). General lesson for other
recomps: this is likely THE generic fix for "CPU-animated billboard stutter"
under RT64 interpolation — enable vertex interpolation with a size threshold
that excludes camera-anchored/regenerated-topology meshes.

## Wave-mesh interpolation — EXPERIMENTAL (2026-07-20), launcher "Smooth Water"

Follow-up to "Cloud stutter — SOLVED": can the WATER also interpolate at high
FPS? Vertex dumps (TEMP WR64_VTXDUMP=1 in rt64_game_frame.cpp, analyzed
offline) show the wave grid is a RIGID BODY in XZ frame-to-frame: every vertex
shares the exact same integer XZ delta (the craft motion, e.g. dX=-63/-64),
dZ~0, while only Y varies (-8..+15, the wave animation). Mathematically ideal
for interpolation — so rt64 d50378a interpolates large meshes when a strict
rigidity check passes (>=98% of verts within 1.0 of the median XZ delta),
with CLEANED velocities (uniform median XZ + per-vertex Y morph; residuals
zeroed against quantization shimmer); rotation frames and cross-paired wave
passes (identical static matrices let similarity matching pair DIFFERENT
meshes — the original "water warp") fail the check and snap.

Telemetry confirms the mechanics (wave chunks 500/867/354 verts accept,
non-rigid pairs reject) — but BY EYE the water motion still doesn't read
right (user-tested twice; sensitivity to camera angle/attract and craft
speed). Parked as EXPERIMENTAL: launcher Enhancements -> "Smooth Water
(experimental)", default OFF (rt64_wr64_set_vertex_interp_rigid;
WR64_VTXINTERP_RIGID=1 env override). Clouds/sprites stay always-smooth.
Open theories for why it reads wrong: texcoords still snap at 20 Hz over
smoothly-sliding geometry (mixed-rate shimmer); the height morph blending
wave PHASE linearly; or the craft/spray interacting with water that now
moves between physics frames.

Also deferred: the top garbage strip (fb rows 0-19, TV-overscan scratch)
exposed by widescreen — a top present-band crop worked but read as a
mismatched black bar; the proper fix belongs to the planned FULL-WINDOW
presentation (game content fills the entire window, overscan cropped as part
of the scale-up). That is the next big presentation milestone.

## Overscan crop = full-window presentation — SOLVED (2026-07-21)

The FULL-WINDOW goal (whole window = game content, no letterbox rectangle, top
junk gone) is delivered via a GLideN64-style overscan crop (user-suggested,
ref github.com/gonetz/GLideN64 OverscanBuffer): per-edge insets are cropped
off the VI image at the FINAL present stage and the remaining content is
scaled up to fill the previous display area — crop WITH zoom (a plain band
crop had been rejected as a mismatched black bar). Because WR64's content
rect is KNOWN — (8,20)-(310,218) of 320x240, and the logical VI space stays
320x240 even under RT64 Expand (fbPair scissors prove it) — the insets are
exact constants, no per-game tuning like GLideN64 needs.

What makes ours better than GLideN64's: PROJECTION COMPENSATION. Cropping
rows/columns stretches the display per axis; the projection processor
pre-scales the projection matrix columns by exactly the inverse (X column *=
(1-l-r), Y column *= (1-t-b), perspective projections in live gameplay only)
so the 3D world keeps IDENTICAL proportions and coverage — only the junk
disappears. 2D/HUD is screen-space and takes the (authentic) TV framing,
i.e. slightly larger.

2P split-screen: the two-band present gained a per-half REMAP — each half's
content band (top src rows 12..120, bottom 122..229 of 240) maps onto its
half of the window with a thin divider (0.8%), side insets x=8..311 cropped
like 1P, and a PER-HALF vertical FOV compensation (rt64_wr64_split_yscale).
Gated by the same toggle (rt64_wr64_set_split_remap); off = classic gutter
bands. User-verified in 2P.

Wiring: rt64 hooks rt64_wr64_set_overscan(l,r,t,b) + xscale/yscale getters +
split remap; port sets per frame (gameplay: all four edges; split: sides
only; menus/stock: zero). Launcher: Enhancements -> "Overscan Crop", default
ON, applies live; WR64_OVERSCAN=0 env kill-switch. The "Show Borders" boot
mode option is now HIDDEN in the UI (superseded; still works via
wr64_settings.json / WR64_BORDERS=1).

Related same-day findings:
- FPS: the frame counter counted GAME frames (DL submissions, ~20 Hz by
  design). Perceived FPS = presents (incl. interpolated) — now counted in the
  fork's VI present (rt64_wr64_consume_present_count) and shown in a new
  in-window top-right overlay pill; the window title is now static-friendly
  ("Wave Race 64 - Recompiled • Target: N FPS • HD Textures: On/Off").
- CRITICAL recompui lesson: a SHOWN context CAPTURES INPUT by default, and
  recompinput disables ALL game input while any shown context captures
  (recompinput game_input_disabled -> recompui is_context_capturing_input).
  The always-on FPS overlay killed in-game keyboard until
  set_captures_input(false)/set_captures_mouse(false). Any future overlay
  context MUST clear both flags.

## Texture packs as mods + F5 toggle (2026-07-21 pm)

Texture packs are now proper mods, exactly the Zelda64Recomp pattern: a
ModContentType keyed on `rt64.json` (runtime-toggleable, on_enabled/
on_disabled/on_reordered -> wr64_mod_texture_pack_*) plus an `.rtz` container
type with requires_manifest=false (a manifest is auto-created from the
filename; `mod.json` + `thumb.png` at the zip root add name/author/version/
icon). `.rtz` is RT64's OWN official pack format (see lib/rt64/
TEXTURE-PACKS.md) — texture_packer builds them zstd-compressed, and our zip
filesystem (rt64_filesystem_zip.cpp) decodes zstd entries, so official packs
load as-is. apply_texture_state_gfx composes ALL enabled packs via
loadReplacementDirectories (which REPLACES the whole set): mod packs sorted
descending by mod-order index + the WR64_TEXPACK env pack last (top
priority). The Textures tab was removed after the mods flow was verified.

F5 toggles all replacements live: RT64's own F4 shortcut is developer-mode
only (sdlEventFilter is gated in the port), so the port polls
SDL_GetKeyboardState edge-detected in update_gfx and flips
textureCache->textureMap.replacementMapEnabled via wr64::
rt64_toggle_replacements() — works during gameplay and menus, no dev mode.
PostMessage'd WM_KEYDOWN updates SDL's keyboard state (SDL translates window
messages), so even unfocused automation can drive it.

Gotchas discovered: (1) a mod's first-scan auto-enable only persists on
clean exit — force-killed runs lose it (mods.json enabled_mods stays empty;
fix by toggling in the Mods tab or editing mods.json); (2) the settings
modal ALWAYS opens on the Controls tab regardless of which launcher entry
launched it (pre-existing set_tab quirk — affects Settings + Mods entries);
(3) launcher list: the first keypress only FOCUSES the list, so keyboard
automation needs Enter x2 to start the game.

## Border Area modes + Aspect Ratio decoupling (2026-07-22)

The Overscan Crop bool became a 3-way "Border Area" enum (Enhancements tab),
ORTHOGONAL to the Graphics Aspect Ratio setting, which previously was a
silent no-op (WR64 permanently forced Expand). Design iterated through four
user-tested attempts — record the landing point, the wrong turns cost hours:

- Graphics -> Aspect Ratio is now REAL and applies live (s_want_43, seeded
  in the ctor, refreshed in update_config). RT64's UserConfiguration STILL
  never flips at runtime (crash/ghosting, see the per-scene presentation
  section) — Original aspect is produced purely by the crop43 present
  pillarbox during gameplay too, exactly like menus.
- Border Area (s_border_mode, WR64_OVERSCAN=0/1/2) picks how the border
  band presents. It NEVER changes the aspect (user was explicit):
  0 Original = black borders. Expand: world stays WIDE (all widening hooks
    unchanged), present = split-band blit rows 20..218 (original-scale
    top/bottom bars) + rt64_wr64_set_frame_sides(frac) black side bars,
    default 6%/side (WR64_FRAME_SIDES=<pct>), deliberately wider than the
    real border columns to cover the expansion-edge artifact zone (see
    below). 4:3: crop43 sub-mode 1 = scissor to the content rect on ALL
    FOUR sides.
  1 Overscan = the 07-21 crop (default, unchanged in Expand incl. 2P
    remap). 4:3: crop43 sub-mode 2 = affine viewport remap, content rect
    exactly fills the 4:3 box.
  2 Extended = experimental raw frame (no crop) in Expand; falls back to
    borders in 4:3.
- Menus follow the setting too: Original -> bordered frame (sub-mode 1),
  Overscan -> content-zoom (sub-mode 2, box filled, no bars), Extended ->
  the legacy 1.08 zoom, which now has a post-zoom vertical clamp to the
  content rows (the zoom hides the SIDE band entirely but only ~4% of the
  ~8-9% tall band — the rest flickered in player select).

KEY PHYSICAL FACT (root cause of two "flickering border" reports): the
border band of the framebuffer is NOT black — it is uncleared RDRAM that a
CRT's overscan hid (the known top-rows-0-19 garbage strip is just its top
edge; the side columns 0..8/310..320 are the same). Any mode that shows the
band region must SCISSOR it to swapchain black, never display it. Attract
mode can look stably black there by luck; racing churns that memory visibly.

Artifact zone (why Original-mode side bars are wide): the border-removal
expansion fills margins with water/sky/tint reliably, but course-world
geometry is gated by the 18-sector PVS (no clip constant to widen — see the
course-world section) and object culling doesn't follow, so the outermost
margins carry blurry seams/stale bands/pop-in. Self-inflicted by the
expansion; the honest presentation is bars over that zone (or Overscan's
crop). Real fixes remain the open neighbor-sector-DL and buoy-cull
experiments.

rt64 hooks (fork e483d77): rt64_wr64_set_crop43_mode(0 menu zoom+clamp /
1 borders inset / 2 content zoom) replacing the old bool, and
rt64_wr64_set_frame_sides(frac). Port: crop43 wanted + widen/dl_widen now
gate on s_want_43 (aspect), not the border mode.

## Motion blur prototype (2026-07-21 pm) — EXPERIMENTAL

Enhancements -> "Motion Blur (experimental)", percent slider (0 = off,
default), WR64_MOTIONBLUR env. Present-time EXPONENTIAL ACCUMULATION: after
the VI blit, the previous PRESENTED frame is alpha-blended over the current
one (alpha = strength) and the blended result is copied back into a
persistent swapchain-sized texture for the next present. Implemented in the
rt64 fork: new WR64MotionBlurPS shader (TextureCopy-style, constant alpha
from a push constant, AlphaBlend pipeline against the swapchain format) +
the accumulate/copy pass in rt64_present_queue.cpp threadPresent, BEFORE the
RmlUi draw hook (launcher/menu UI never blurs; in-game HUD is part of the
game frame, but static HUD pixels don't change so they stay sharp — only
moving content trails). rt64_wr64_set_motion_blur(strength), capped 0.95.

Status: prototype, KEPT EXPERIMENTAL — user-verified it works but "breaks
some stuff" visually (accumulation smears everything that moves, including
legitimate flicker effects; known limitation of the approach, not a bug to
chase). Proper per-object motion blur would need a per-pixel velocity
G-buffer in RT64's raster path (RT64 computes per-vertex velocities CPU-side
for interpolation but never rasterizes them) — new render target + shader
permutations + a directional-blur post pass; weeks-scale, heavy fork
divergence. Camera-only blur (reconstruct motion from depth + previous
frame's matrices, which RT64 already tracks) is the plausible middle option
at days-scale.

## Animated launcher background (2026-07-21 pm)

The static wr64_background.svg was split into layers, all generated by
scratchpad make_bg_layers.py (same art style): sky+sunglow base (via
set_launcher_background_svg), sun disc + glitter, clouds, two bird layers,
three wave bands, spray bubbles, vignette (static, keeps menu text
readable). Layers are stacked in the launcher's background_wrapper
(get_background_wrapper()) with the same centering style as the stock
background (top 50%, translate Y -50%, width %).

Animation: WR64BackgroundAnimator (main.cpp), an Element with
EventType::Update events. KEY recompui detail: update events are ONE-SHOT —
queue_update() in the ctor and re-queue at the end of every handler (the
GameModeOption pattern). It only receives updates while the launcher context
is shown => zero in-game cost. Waves: drawn TWO PERIODS wide (200% width,
period = window width), so drifting `left` over [-100%, 0) loops seamlessly;
each band gets its own drift speed (parallax) + vertical bob via
translate-Y% on offset sine phases. Sun: opacity pulse + tiny bob. Spray:
opacity shimmer + sway. Clouds: slow drift. Birds: gull silhouettes in TWO
wing poses (wr64_bg_birds_a/b.svg), crossfaded at ~2.6 Hz for a flap effect,
gliding faster than the clouds behind them.

Second pass (same day, user request): island + palm (static, behind the far
wave so the surf laps at it), TWO DOLPHINS (one layer each) doing
periodic jump arcs — the art sits DEEP (y~830/900) so the mid wave in front
OCCLUDES it at rest; a jump is a parabolic translate (sin arc up + linear
forward travel, 9 s cycle, phase-offset, opposite directions) so they break
the surface, arc over with spray droplets, and dive back — no opacity tricks
(v2; the first version faded them in/out, which read as popping), a JET SKI with rider racing rightward
between mid/near waves (left% drift at 11%/s, mid-wave swell bob + fast chop
bounce; two copies 1600 apart = seamless), a layered SUN AURA (wide radial
glow + bright core), and a LENS FLARE layer above the water (anamorphic
horizontal streak through the sun + ghost chain up the flare axis), whose
opacity breathes on two superimposed sine periods (6 s + 17 s).

## CI release build — toolchain reality (2026-07-21)

Notes on building this project from a clean checkout in CI (the release
workflow itself lives outside this repo). Getting a green build surfaced a
stack of local-vs-CI gaps and a runner regression — all worth not
rediscovering:

- **Three inputs the public repo does NOT commit, regenerated in CI:**
  RecompiledFuncs/ + rsp/aspMain.cpp (from N64Recomp/RSPRecomp over the ROM),
  and **patches/syms.ld** (gitignored; `scripts/generate_syms_ld.py` from the
  syms TOML — the patches Makefile depends on it but has no rule to make it).
  NB local dev only regenerates syms.ld when the script is run by hand; if the
  syms TOML changes, rerun it (rebuild.cmd does NOT).
- **N64RecompCLI** is the exe target (produces N64Recomp.exe); plain
  `N64Recomp` is the library — building the wrong one "succeeds" without
  emitting the exe.
- **THE RUNNER REGRESSION (root cause of two failures):** `windows-latest`
  moved to VS 18 / Clang 20. (a) MSVC STL asserts "expected Clang 20 or newer"
  (STL1000) — portable LLVM 19.1.3 clang-cl fails it. (b) Clang 20 rejects
  SDL 2.26.3's hand-rolled `_m_prefetch` shim ("definition of builtin
  function"). Verified across Zelda64 / Banjo / Trouble-Makers: ALL bundle
  SDL 2.26.3 (upstream rt64 still does, even our 2026-05 base; TM's pinned rt64
  23cab603 is ~9 months OLDER than ours, same SDL) and all built on older
  Clang < 20 images — nobody avoids `_m_prefetch` by config; their green builds
  are historical. FIX = pin `runs-on: windows-2022` (VS 2022, Clang ≤ 19):
  tolerates the SDL shim AND the runner's VS clang matches its own STL. Bonus:
  freezes the toolchain for reproducibility.
- **Compiler split (now unanimous across all reference recomps):** main build
  uses the runner's VS-bundled clang-cl (found via `vswhere -find
  VC\Tools\Llvm\x64\bin\clang-cl.exe`); portable LLVM 19.1.3 clang + ld.lld
  only for the MIPS patches (their GCC-style flags need it). Added
  `-Xclang -fexceptions -Xclang -fcxx-exceptions` (Zelda + Banjo both pass it;
  recompui/RmlUi use exceptions) and `-DCMAKE_MT=mt` (portable llvm-mt lacks
  libxml2, dies "no libxml2" at exe-link manifest embed).
- **CI caught real missing-committed sources:** patches/ui_funcs.h +
  patch_helpers.h + recompui_event_structs.h were untracked (present locally,
  never committed) — a fresh clone couldn't build. Committed (see below).
- Package from BUILD OUTPUT (CMake stages SDL2/dxil/dxcompiler + assets next to
  the exe via POST_BUILD) + MSVC runtime from System32. Zip name carries a UTC
  timestamp + short source SHA, with a .sha256 sidecar.
- YAML trap: no column-0 PowerShell here-string (`@"`/`"@`) inside a `run:`
  block — it breaks the workflow parse; use a PS array instead.

## CI Linux build — toolchain reality (2026-07-22)

Second CI target after Windows, modeled on BanjoRecomp/Zelda64Recomp's Linux
job. Reached green in three iterations; each failure was a
Windows-only-ism the port had never had to compile on POSIX:

- **Toolchain:** ubuntu-22.04, a single `clang-15` for BOTH the host build
  and the MIPS patches (no portable-LLVM split like Windows needs — one clang
  does both; `-DPATCHES_C_COMPILER=clang-15 -DPATCHES_LD=ld.lld-15`). SDL2
  2.30.3 built from source (the distro's 2.0.20 is too old for the 2.30 APIs
  the port links) and copied into the multiarch lib dir. apt deps:
  `ninja-build libgtk-3-dev libfreetype6-dev lld llvm clang-15` (GTK = NFD
  file dialogs, freetype = RmlUi). The port's CMake uses
  `find_package(SDL2)` on non-Windows (Windows FetchContents the VC zip).
- **FAIL 1 — `libdxcompiler.so: cannot open`.** rt64's own shader rules
  prefix the non-Windows `dxc-linux`/`dxc-macos` invocation with
  `LD_LIBRARY_PATH=<contrib>/dxc/lib/x64` (custom commands run via sh, so a
  leading `VAR=…` token in the command list works). Our top-level CMake
  re-exports a `DXC` variable for the RecompFrontend shader rules and had
  pointed it at the BARE binary — so recompui's InterfaceVS/PS.hlsl failed to
  compile. Fix: mirror rt64's `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` prefix in
  our re-export (CMakeLists, `if BUILD_RT64` block). Windows unaffected.
- **FAIL 2 — recompui `no viable overloaded '='` on `appCore.window =
  window_handle`.** rt64 sets `PLUME_SDL_VULKAN_ENABLED` /
  `RT64_SDL_WINDOW_VULKAN` with DIRECTORY-scoped `add_compile_definitions`,
  which recompui (added via a different `add_subdirectory`) does NOT inherit.
  Without them `plume::RenderWindow` falls back to the raw X11
  `{Display*, Window}` struct on Linux, while ultramodern's `WindowHandle` is
  `SDL_Window*` → type mismatch. Fix: `target_compile_definitions(recompui
  PRIVATE PLUME_SDL_VULKAN_ENABLED RT64_SDL_WINDOW_VULKAN)` after the
  RecompFrontend add_subdirectory, gated on the same Linux+SDL-Vulkan
  condition rt64 uses.
- **FAIL 3 — `use of undeclared identifier '_putenv_s'`** (our own
  src/rt64_render_context.cpp, the WR64_HUD=stretch → RT64_RECT_ASPECT_DEFAULT
  handoff). `_putenv_s` is Windows CRT; wrapped `#ifdef _WIN32` with a POSIX
  `setenv(name,val,1)` else-branch. Grepped src/ for the other usual CRT-only
  suspects (`CreateDirectoryA` etc.) — the rest were already `#ifdef _WIN32`
  gated.
- **Renderer = Vulkan** on Linux (SPIR-V shaders precompiled at build), so the
  package needs NO dxcompiler/dxil runtime libs — just the ELF + assets +
  bundled libSDL2 as a fallback for distros with an SDL2 older than 2.30
  (`LD_LIBRARY_PATH=. ./WaveRace64Recomp`). Versioned
  `WaveRace64Recomp-Linux-X64-<ts>-<sha>.tar.gz` + .sha256.
- The release workflow gained a `platforms` dispatch input (all/windows/linux)
  so a single job can be iterated without also burning the 2x-billed Windows
  runner; the `release` job now needs both builds and attaches the tarball.

macOS: NOT built yet. Trouble-Makers (Mischief Makers) does NOT build macOS
either — only Linux+Windows — so the sole reference is BanjoRecomp
(macos-14 runner, MacPorts clang-18/llvm-18 for the MIPS patches, universal
`-DCMAKE_OSX_ARCHITECTURES="x86_64;arm64"`, packages a .app). The Metal
shader path (MSL via dxc/spirv-cross) is the main unknown for our WR64-fork
present shaders.

## CRT filter (Trinitron) — present pass + content-rect plumbing (2026-07-22)

Enhancements → "CRT Filter (0% = off)" / `WR64_CRT=<0-100>`: single-pass
Trinitron look (aperture grille, scanlines, barrel curvature, rounded
corners, vignette, brightness compensation), one intensity slider scaling
the whole tuned look. Same pattern as motion blur/sharpen — a present-time
pixel shader in the rt64 fork — with one NEW piece of infrastructure worth
remembering:

- **Content-rect plumbing** (`rt64_wr64_get_content_rect` +
  `rt64_wr64_get_content_src_rows`, rt64_vi_renderer.cpp): the present-queue
  passes previously had NO access to where the game image sits in the
  swapchain (sharpen/blur are full-frame, so it never mattered). The VI
  renderer now publishes the final blit rect per present — after crop43
  pillarbox, frame-side bars, and band blackout — plus the number of visible
  SOURCE rows (198 content rows vs 240 full frame). The CRT shader maps its
  curvature/corners onto that rect ("tube"), so black bars stay flat in
  every Border Area / Aspect Ratio combination, and 2P split-screen gets ONE
  tube spanning both halves (like the real console on a real CRT). Scanline
  pitch is locked to source rows, not output pixels.
- **Pass order**: VI blit → sharpen → motion blur → **CRT** → UI hook. CRT
  last keeps the phosphor mask off the blur's accumulation history (the mask
  "sits on the glass") and off the launcher/overlays.
- **Linear sampling**: curvature needs smooth UVs; TextureCopyDescriptorSet
  has no sampler, so the CRT pipeline reuses the VideoInterfaceDescriptorSet
  shape (texture t1 + immutable linear sampler s2) — no new descriptor-set
  type needed. It shares the sharpen pass's scratch texture (runs after it).
- **Resolution-adaptive**: grille pitch = max(3 px, ~0.75 × output pixels
  per source scanline) so the mask scales with window size instead of
  vanishing at 4K; below ~3 px per scanline/triad both masks fade out
  (moiré guard) and small windows degrade to curvature+vignette.
- Tuned strengths at 100%: grille 0.40, scanlines 0.35, curvature 0.045,
  corner radius 0.02+0.06k, vignette 0.12, gain cap 1.35.

Phosphor simulation extended (2026-07-22, same slider): the aperture grille
already IS the RGB-phosphor-stripe simulation; added the things beyond it
that sell the tube — P22 phosphor color (3x3 channel-crosstalk toward CRT
primaries + CRT gamma, both lerp'd by k), phosphor glow/halation, and (later
made an independent toggle) a physical TV bezel with reflection. This round
surfaced two real regressions, both found by deliberately trying to break
the effect with a live window resize rather than trusting a single "looks
fine" screenshot — worth internalizing as a testing habit for any future
present-time effect with a persistent buffer.

**Glow/halation is now computed INLINE in the CRT shader, not a separate
render target.** The first implementation (WR64GlowPS) bright-pass-blurred
the frame into a separate quarter-res color target once per present, which
the CRT shader then sampled. That target's lazy (re)allocation was keyed on
SIZE only — and a live window drag-resize rebuilds the swapchain almost
every present, so a transient size match left the glow target and its
descriptor set bound to stale/recycled VRAM. The result: a permanent hazy
overlay that never cleared, visible after ANY manual resize (not just
fullscreen<->window — plain drag-resize on either axis reproduced it, user
caught it after a vertical resize specifically). Diagnosis method: bisect by
disabling one contributing factor at a time (CRT off entirely → clean;
persistence off → still broken; ergo the glow target) rather than guessing
from one screenshot — the same lesson as below. **Fix: deleted the separate
glow target/pass and shader entirely** — `CrtHalation()` in WR64CrtPS.hlsl
now does its own 12-tap bright-passed blur (threshold 0.60, soft knee)
directly off the existing scratch-copy texture, sampled at the distorted
position and added at 0.45*k. One texture, one pass, nothing to
reallocate-out-of-sync with the swapchain generation.

**Phosphor persistence defaults OFF** (`WR64_CRT_PERSIST=1` opts back in,
`rt64_wr64_get/set_crt_persist`). It reuses the existing motion-blur
accumulation buffer (`blurK = max(motionBlur, crt*0.12)`) — that buffer
also never fully recovered after a drag-resize in testing, so rather than
chase it further the trail (always a "kept minimal deliberately" nice-to-
have, not core to the CRT look) was switched to opt-in.

**Belt-and-suspenders against the whole class of bug**: swapchain
framebuffer (re)creation (`rt64_present_queue.cpp`, where
`swapChainFramebuffers.empty()`) now drops ALL present-effect resources —
scratch, blur history, CRT descriptor set — not just the ones a given bug
touched, plus a one-shot `wr64SkipEffectsOnce` flag that sits the very next
present out entirely. Belt-and-suspenders because the inline-halation fix
above already removes the specific reallocation race; this is defense
against the *next* present effect someone adds making the same mistake.

**CRT Bezel** (Enhancements → "CRT Bezel", `WR64_CRT_BEZEL=0`, default on,
`rt64_wr64_get/set_crt_bezel`): a TV frame with actual depth, independent
on/off at FIXED strength — does NOT fade with the CRT Filter intensity
slider, so a low-intensity CRT look still gets a full-strength frame.
Three iterations to get the geometry right, each one a real user-caught
visual bug, worth remembering for any future rounded-rect/frame shader:

1. **Depth shading**: the frame "walls" face the tube center and are lit
   from below (like a real recessed console screen) — computed from the
   outward normal of the rounded-rect SDF (`dirO`), not just a radial
   falloff, so the top of the frame reads as shadowed and the bottom as lit
   exactly like the reference photo the user linked.
2. **Corner geometry must be computed in PIXEL space, not normalized tube
   space.** The rounded-rect SDF was first written in the [-1,1] tube
   space; on a wide window that space is anisotropic (squashed >2:1), which
   turned the corner ARCS into flat elliptical chamfers — straight
   45-degree cuts to the eye — and made the frame visibly thicker on the
   sides than top/bottom. Rewritten with `halfPix = tubeSize * 0.5` and the
   SDF evaluated in pixels: circular arcs and uniform frame width at any
   aspect ratio.
3. **The frame opening must be defined in the DISTORTED (barrel-warped)
   image space, with straight edges exactly on the image boundary and
   corner arcs cutting INTO the image, not into the surrounding black.**
   Cutting the opening in flat/undistorted space left a black gap between
   the barrel-shrunk image corners and the (larger, un-shrunk) frame
   opening — visible as a dark "moat" in every corner, worse on bright
   scenes where it read as an obviously wrong void. The fix models a real
   TV: the picture tube sits UNDER the bezel, so the opening's geometry is
   anchored to the image itself (`qd = (abs(d) - 1) * halfPix + cornerR`),
   which by construction cannot show a gap regardless of window aspect.
   Bright corner content also used to spill PAST the rounded opening as a
   squared-off white ledge (the reflection sampled undistorted screen-space
   color without accounting for the corner curve) — fixed together with a
   dark "glass gap" ring (`smoothstep(0.03, 0.28, t)`) that keeps the
   reflection from starting until past the glass edge, and Reinhard
   tone-compression on the reflection color (`x / (1 + 2.5x) * 1.4`) so a
   reflected sky/surf highlight can't reach full white and paper over the
   rounded corner the way raw HDR-ish values did.
   Corner-radius and glass-gap constants ended up FIXED (not scaled by CRT
   intensity) once the bezel became its own toggle — a partial-intensity
   CRT effect with a partial-strength frame looked inconsistent.
4. Vignette (separate from the bezel, part of the base CRT look) capped at
   -22% instead of scaling unbounded with `r2^2` — at the true tube corners
   `r2` reaches ~2.0, which without a cap sank the corners into a near-black
   pit that fought visually with the bezel's own corner shading.

Deferred: per-component env fine-tuning knobs. NOTE the whole CRT + sharpen
+ motion-blur present-effect suite (plus the bezel) is fork-clean and a
plausible upstream RT64 contribution later (self-contained passes +
content-rect plumbing).

**Ghosting on fullscreen<->window (fixed, separate from the resize bug
above)**: the persistence/motion-blur accumulation kept `wr64PrevFrame`
keyed only on swapchain size. A FS<->window toggle re-lays-out the game
image (different position/scale) while dimensions can momentarily match, so
the stored frame blended in as stretched ghost duplicates (user saw doubled
logo/text). Fix: also track the captured frame's content rect and drop
history when it changes. Any future accumulation-style present effect must
invalidate on layout change, not just resize.

**Debugging method note**: nearly every fix in this section came from
refusing to trust a single screenshot and instead (a) automating a repro
(PostMessage-driven fullscreen toggle / SetWindowPos drag-resize bracketed
by WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE + F12) and (b) bisecting by disabling
one suspect at a time (`WR64_CRT=0`, `WR64_CRT_PERSIST=0`) until the
minimal reproducing configuration was found. Several early "fixes" (content
-rect tracking alone, a skip-once flag alone) looked plausible and were
WRONG — they didn't reproduce-test clean before being reported as fixed.

Stable tube + N64 logo (2026-07-23): the CRT/bezel tube must NOT change
shape between scenes — a simulated TV frame doesn't move. It originally
followed the per-scene VI rectangle, so the 4:3 N64 boot logo (VI rect
1066x800, native 4:3) drew a 4:3 bezel before the wide attract/gameplay
(user reported "N64 logo bezel still 4:3"). Diagnosed via a `[VITUBE]`
per-frame rect-dims log (NOT screenshots — the logo is a sub-second
transient the F12 shot kept missing, and F12'ing that early crashed the
toast context, see below): frames 1-5 logged ar=1.332, frame 6+ ar=2.400.
Fix: `rt64_wr64_set_tube43` — in Expand the tube is clamped to the FULL
window (the narrower scene pillarboxes INSIDE the fixed glass); in Original
it is the fixed 4:3 crop box. getViewportAndScissor stashes the pre-crop
base scissor + the 4:3 box so the tube publish picks the stable one
regardless of the per-scene crop43 state. Lesson: for a sub-second early
scene, LOG the values every frame; don't chase it with screenshots.

Crash while debugging (2026-07-23), two findings: (1) the minidump handler
wrote a 0-byte file — MiniDumpWriteDump's return was never checked, and in
a multi-threaded process the writer can fail if other threads mutate memory
mid-walk. Fixed: suspend all other threads (Toolhelp snapshot) before the
dump + check/log the result. (2) The crash itself was MY automation, not a
game bug: rapid F12 within the first second hit `recompui::create_context`
(lazily created on the first screenshot TOAST) before recompui was ready —
null deref in create_empty_document. With the /Z7+/DEBUG symbols the dump
now resolves the full stack. Takeaway: don't drive F12 until the game is
past init.

Sky colour banding — NOT fixable by a present-time dither (2026-07-23):
tried a final-stage dither (rectangular then TPDF, up to ~1.5 LSB), user
saw no difference and it was removed. Reason: the banding is baked in at
the game's native 16-bit (RGBA5551) framebuffer during rendering — a dither
AFTER that can't recover thrown-away precision, only barely touch the final
8-bit requantization. The real lever is rendering the internal framebuffer
above 16-bit: RT64's `internalColorFormat` (High/Automatic), exposed as the
Graphics tab "High Precision Framebuffer" option — which was present but
registered `hidden=true` (serialized to graphics.json, never shown). Un-hid
it and defaulted it to Auto. LESSON: for gradient banding, fix the render
bit depth, not the output — a post dither is the wrong tool.

## CRT Bezel — overlay image, not procedural (2026-07-23)

The procedural shader-drawn bezel (SDF rounded box + bevel/reflection, above)
never read as a real TV housing across many rounds of tuning — a shader can't
match a photo of a real set. Replaced with the RetroArch/Mega-Bezel approach:
an **overlay-image** bezel composited in the CRT present pass.

- Asset `assets/crt_bezel.png` is generated by a Pillow script (kept in the
  session scratchpad, `make_bezel.py`; NOT committed — only the PNG is): a
  boxy near-black matte frame with a transparent centre cutout, a directional
  3D bevel, a thin lit inner rim, and a soft inner cast-shadow onto the glass.
  Original art — the JVC/Panasonic reference photos the user supplied are
  copyrighted and were used only as visual targets.
- Compositing lives in `WR64CrtPS.hlsl`: the game is shrunk into an inset
  "glass" rect (`INSET_X/INSET_Y`, MUST match the generator's cutout inset),
  and the PNG is sampled across the full presented rect and `lerp`'d over the
  game by its alpha. rt64 side: the PNG is decoded once via
  `TextureCache::loadTextureFromBytes` (stb PNG path) in `rt64_present_queue`
  and kept resident (survives swapchain rebuilds); a new `WR64CrtDescriptorSet`
  adds a `gBezel` texture slot to the CRT pass.
- **Bug — a new descriptor-set texture read all zeros.** Adding `gBezel` as
  `addTexture(1); addImmutableSampler(2); addTexture(3)` (texture/sampler/
  texture interleaved) made the shader sample black. Every working multi-
  texture set in the tree groups textures FIRST then the sampler
  (`PostProcessDescriptorSet`: t1,t2,t3 then s4). Reordering to
  `addTexture(1) gInput; addTexture(2) gBezel; addImmutableSampler(3)` (shader
  `t1`,`t2`,`s3`) fixed it. Verified with a shader debug that output `b.a`/`b.g`
  directly. Lesson: mirror the existing descriptor-set layout ordering exactly.
- **Corners:** using the shader's rounded `tubeMask` to clip the game AND a
  square PNG cutout left chamfered black wedges in the corners. Fix: with the
  bezel on, draw the game as a plain rectangle (no `tubeMask`); the PNG cutout
  alone defines the visible corners. Also the bevel's directional light
  (`ox/oy` via `np.where` per edge) JUMPED at the corners → a hard miter seam
  baked into the PNG; replaced with a smooth rounded-rect SDF-gradient outward
  (`sign(p)*max(q,0)` with a corner-smoothing radius) so the lighting rotates
  gently through the corners while the glass stays square.
- **Curvature follow:** when the CRT filter is on, the game barrel-curves;
  sampling the bezel at the SAME curved position (`d`) makes the frame opening
  follow the curve (flat when the filter is off, since curve→0 → `d==c`). But
  the plain barrel `c*(1+curve*r²)` pushed the cutout edge inward and fattened
  the thin frame; NORMALISING it `/(1+curve)` anchors the edge midpoints
  (|c|=1 → d==c) so only the shape bows and the frame keeps its thickness.
- **Reflection:** the frame picks up the screen colour at the nearest glass
  edge (scale-to-fit inverse), heavily blurred (`EdgeGlow`, 4 rings out to
  ~220 px) for a diffuse wash, confined to the inner ~60 % of the band and
  tapered in the corner squares so there are no hard colour blocks.
- Iteration lesson (many rounds): the user's terse visual notes each pinned ONE
  variable — "too round", "white oval outline", "too fat", "hard gradients at
  the corners", "reflection not visible", "more diffuse". Change one thing,
  re-capture, confirm. Env `WR64_CRT_BEZEL=1` now force-ENABLES too (was 0-only)
  so autonomous runs can isolate it regardless of saved config.
- **Art refinement round (2026-07-24):** the user iterated the PNG toward a
  flat picture-frame — uniform brightness all around (directional bevel light
  REMOVED, it read as "brighter top-left"), inner bevel a touch brighter than
  the outer flat, mitred-corner seam confined to the inner bevel only, and the
  inner bevel's outer corners made TRUE 90° by driving the bevel profile off a
  BOX (Chebyshev max) distance instead of the euclidean SDF (euclidean rounds
  corners). Asymmetric inset (INSET_X 0.026 < INSET_Y 0.035) + a wider L/R
  inner cast-shadow makes the picture read as recessed/deeper on the sides.
- **Reflection sampled the extreme edge = wrong (2026-07-24 fix):** with the CRT
  curve or a pillarboxed menu the very edge of the presented rect is often black
  (curved-out corner / bar), so the frame reflected BLACK. Fix (user's idea):
  sample the reflection from the OUTER ~10 % band of the game image, not the
  edge — inset the sample point ~6 % inward (`rTubeInset = 0.06 + 0.88*rTube`);
  the wide EdgeGlow blur then averages that real edge band. Blur widened over
  several rounds (rings out to ~1000 px) for a very diffuse wash.
- OPEN: the user still wants the PNG art refined further (deferred).

## macOS .app bundle (2026-07-22)

Once the arm64 CI went green (5 port fixes, see the CI Linux/macOS notes),
the bare binary linked MacPorts dylibs from /opt/local — not portable. Now
a self-contained `.app` like Zelda64Recomp/BanjoRecomp:
`.github/macos/apple_bundle.cmake` (MACOSX_BUNDLE, Info.plist.in,
entitlements.plist, .icns from resources/wr64_icon_preview.png best-effort)
+ `fixup_bundle.cmake` (BundleUtilities `fixup_bundle` copies SDL2/freetype
into Contents/Frameworks and rewrites load paths), assets into
Contents/Resources, `@executable_path/../Frameworks` rpath, ad-hoc
`codesign` with the entitlements. Included from the CMakeLists APPLE block
(which also links Threads::Threads + dl). Release job zips the .app with
`zip -y` (preserves framework symlinks).
- **NO ld64 max_prot wrapper** (Zelda/Banjo ship one): that exists for
  runtime mod FUNCTION patching (writable+executable memory). WR64's
  recompiled code is AOT and its mods are texture packs only, so no W+X
  segments are needed — skipped. The exec-memory entitlements are kept for
  parity; revisit only if a launch fails on memory protection.

## recompui bridge headers (patches/{ui_funcs,patch_helpers,recompui_event_structs}.h)

The game↔frontend UI contract, hand-written, game-specific (so they live in the
game repo's patches/, not in generic RecompFrontend). recompui reaches into them
via `#include "../../../../../patches/ui_funcs.h"`.

- **patch_helpers.h** — the dual-ABI glue. `DECLARE_FUNC(type, name, ...)`
  expands differently per side: under `MIPS` (patch code → N64 elf) it's a
  normal `extern "C" type name(args)`; on the native side EVERY recompiled
  function has the uniform recompiler signature `void name(uint8_t* rdram,
  recomp_context* ctx)`. Lets one declaration mean the right thing to both
  compilers. Standard N64Recomp pattern (Zelda/Banjo have the same helper).
- **recompui_event_structs.h** — shared UI event vocabulary (RecompuiEventType
  click/focus/hover/drag/menu-action…, RecompuiMenuAction, the RecompuiEventData
  tagged union). Both sides need identical layout to pass events across.
- **ui_funcs.h** — includes the two above and declares the bridge entry point
  `recomp_run_ui_callbacks` via DECLARE_FUNC. recompui itself DEFINES that
  function (ui_api_events.cpp), and #includes the host's ui_funcs.h to get the
  matching declaration + the shared event-struct/patch_helpers vocabulary.
  So these headers are a host-provided CONTRACT that recompui hardcodes an
  include of (`../../../../../patches/ui_funcs.h`) — NOT game-side
  implementations. WR64 implements no custom game-side UI callbacks
  (patches/placeholder.c is empty); the port drives the launcher natively
  from main.cpp. This host-supplies-the-contract coupling is a Zelda64Recomp
  template convention, inherited via RecompFrontend — every host repo must
  ship these small headers, and recompui_event_structs.h must stay in sync
  with recompui's own ui_types.h enums (there's a comment there saying so).
