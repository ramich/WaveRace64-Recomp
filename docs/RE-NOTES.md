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

## Frame interpolation & the cloud stutter — INVESTIGATED (2026-07-18), unresolved

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

### KNOWN-ISSUES (confirmed + characterized, not yet fixed)
1. **Flyby lower-half ghosting** — during the fast pre-race intro camera pan,
   the BOTTOM half ghosts. Transient, motion-only; not visible in stills
   (PrintWindow grabs one composited frame). Almost certainly split-viewport
   frame-interpolation (same class as the cloud-stutter thread). Deep.

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
