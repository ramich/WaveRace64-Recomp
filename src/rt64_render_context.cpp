/**
 * @file rt64_render_context.cpp
 * @brief RT64 RendererContext implementation for WaveRace64-Recomp.
 *
 * Wraps the RT64::Application into an ultramodern::renderer::RendererContext
 * subclass so that the N64ModernRuntime can drive display list submission,
 * screen updates, and shutdown through a uniform interface.
 */

#ifdef _WIN32
// RT64's D3D12 headers (dxcapi.h etc.) need the COM base declarations
// (IUnknown, IStream) before they are included.
#include <windows.h>
#include <unknwn.h>
#include <objidl.h>
#endif

#include "rt64_render_context.h"

#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <algorithm>
#include <atomic>
#include <vector>

#include <SDL2/SDL_events.h>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/config.hpp"

#include "hle/rt64_application.h"

#ifdef HAS_RECOMPUI
#include "recompui/recompui.h"
#endif

// Exported by lib/rt64 rt64_rsp.cpp (WR64 patch): segment-3 projections that
// geometry was actually drawn under, split into live-world cameras vs the
// menus' parked-world fovy=50 camera — drives the scene classifier in send_dl.
extern "C" uint32_t rt64_wr64_world_proj_loads();
extern "C" uint32_t rt64_wr64_menu_world_proj_loads();
// Exported by lib/rt64 rt64_vi_renderer.cpp (WR64 patch): pillarbox the final
// present blit to 4:3. Aspect config stays Expand permanently.
extern "C" void rt64_wr64_set_present_crop43(int enabled);
extern "C" void rt64_wr64_set_scissor_widen(int enabled);
extern "C" void rt64_wr64_set_scissor_widen_mask(uint32_t mask);
extern "C" uint32_t rt64_wr64_scissor_match_count();
extern "C" void rt64_wr64_set_viewport_widen(int enabled);
extern "C" void rt64_wr64_set_wide_world(int enabled);

// ---------------------------------------------------------------------------
// Static dummy buffers required by RT64 (must persist for the lifetime of app)
// ---------------------------------------------------------------------------

// ROM header placeholder — RT64 reads the first 0x40 bytes for cartridge info.
// We don't need real header data for HLE rendering.
static uint8_t s_dummy_rom_header[0x40] = {};

// SP DMEM/IMEM — RT64's RSP HLE doesn't use these but the core struct must be
// non-null to avoid null-dereferences inside RT64's state setup paths.
static uint8_t s_DMEM[0x1000] = {};
static uint8_t s_IMEM[0x1000] = {};

// RDP / MI register storage — RT64 may read/write these during HLE rendering.
// Providing real zero-initialised storage prevents nullptr dereferences.
static unsigned int s_MI_INTR_REG     = 0;
static unsigned int s_DPC_START_REG   = 0;
static unsigned int s_DPC_END_REG     = 0;
static unsigned int s_DPC_CURRENT_REG = 0;
static unsigned int s_DPC_STATUS_REG  = 0;
static unsigned int s_DPC_CLOCK_REG   = 0;
static unsigned int s_DPC_BUFBUSY_REG = 0;
static unsigned int s_DPC_PIPEBUSY_REG= 0;
static unsigned int s_DPC_TMEM_REG    = 0;

// No-op interrupt check — the recompiler runtime handles interrupts itself.
static void dummy_check_interrupts() {}

// Live application pointer for event forwarding (single renderer instance).
static std::atomic<RT64::Application*> s_app{nullptr};
static std::atomic<bool> s_tex_pack_loaded{false};

// Texture-replacement state, driven by the launcher "Textures" tab (setters
// below) and WR64_TEXPACK/WR64_TEXDUMP env overrides. UI-thread setters only
// touch these + raise s_tex_dirty; the actual RT64 mutation happens on the gfx
// thread in apply_texture_state_gfx() (called from update_screen).
static std::mutex        s_tex_mutex;                 // guards the string members
static std::string       s_tex_pack_dir;              // pack folder ("" = none)
static std::string       s_tex_dump_dir = "textures_dump"; // hardcoded dump output
static std::atomic<bool> s_tex_replace_enabled{true}; // enable replacements
static std::atomic<bool> s_tex_dump_enabled{false};   // dump textures
static std::atomic<bool> s_tex_dirty{false};          // runtime re-apply request

// Reconcile RT64's texture state with the launcher/env settings. MUST be called
// on the gfx thread (loads packs / mutates textureCache + state, which the
// render path also touches). Cheap no-op unless s_tex_dirty is set. Only forces
// replacementMapEnabled on an apply, so F4 can freely toggle in between.
static void apply_texture_state_gfx(RT64::Application* app) {
    if (app == nullptr || !s_tex_dirty.exchange(false)) {
        return;
    }
    std::string want_pack, dump_dir;
    {
        std::lock_guard<std::mutex> lk(s_tex_mutex);
        want_pack = s_tex_pack_dir;
        dump_dir = s_tex_dump_dir;
    }
    // (Re)load the pack when the folder changes.
    static std::string applied_pack;  // gfx-thread-only
    if (app->textureCache != nullptr && want_pack != applied_pack) {
        applied_pack = want_pack;
        s_tex_pack_loaded.store(false);
        if (!want_pack.empty()) {
            std::error_code ec;
            // Accept either a folder or a .zip file — RT64's loader picks
            // FileSystemZip for a regular file (miniz) and FileSystemDirectory
            // for a folder. Both need an rt64.json inside (or, for Rice packs,
            // the Rice database).
            const bool is_dir = std::filesystem::is_directory(want_pack, ec);
            const bool is_file = std::filesystem::is_regular_file(want_pack, ec);
            if (is_dir || is_file) {
                if (app->textureCache->loadReplacementDirectory(RT64::ReplacementDirectory(want_pack))) {
                    s_tex_pack_loaded.store(true);
                    fprintf(stderr, "[WR64-TEX] texture pack loaded (%s): %s\n",
                            is_file ? "zip" : "folder", want_pack.c_str());
                } else {
                    fprintf(stderr, "[WR64-TEX] texture pack failed to load (no rt64.json?): %s\n", want_pack.c_str());
                }
            } else {
                fprintf(stderr, "[WR64-TEX] texture pack not found: %s\n", want_pack.c_str());
            }
        }
    }
    if (app->textureCache != nullptr) {
        app->textureCache->textureMap.replacementMapEnabled =
            s_tex_replace_enabled.load() && s_tex_pack_loaded.load();
    }
    // Dumping: hardcoded output dir (dump_dir; env WR64_TEXDUMP=<path> can override).
    if (app->state != nullptr) {
        const bool dump = s_tex_dump_enabled.load();
        if (dump && app->state->dumpingTexturesDirectory.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(dump_dir, ec);
            app->state->dumpingTexturesDirectory = dump_dir;
            fprintf(stderr, "[WR64-TEX] dumping textures to: %s\n", dump_dir.c_str());
        } else if (!dump && !app->state->dumpingTexturesDirectory.empty()) {
            app->state->dumpingTexturesDirectory.clear();
            fprintf(stderr, "[WR64-TEX] texture dumping stopped\n");
        }
    }
}

// ---------------------------------------------------------------------------
// WR64-specific game settings — configurable via launcher UI or env vars.
// Statics are module-internal; setters are called by the launcher callbacks.
// ---------------------------------------------------------------------------
static std::atomic<bool> s_show_borders{false}; // false = border removal ON (default)
static uint32_t s_wavegrid_rows = 23;    // default wide wave grid
static uint32_t s_wavegrid_cols = 55;
static float    s_fov_degrees   = 47.75f; // matches toml patches; 0 = don't poke

// Present-crop request latch, shared by the border-removal scene classifier
// and the borders-on reset path so a border toggle re-sends the crop state.
// -1 unknown, 0 menu (cropped 4:3), 1 gameplay (wide).
static int s_crop_requested = -1;

void wr64_set_show_borders(bool show)                        { s_show_borders  = show; }
void wr64_set_wavegrid(uint32_t rows, uint32_t cols)         { s_wavegrid_rows = rows; s_wavegrid_cols = cols; }
void wr64_set_fov_degrees(float deg)                         { s_fov_degrees   = deg;  }

// Launcher "Textures" tab setters (UI thread). They only stage state + raise the
// dirty flag; apply_texture_state_gfx() applies it on the gfx thread.
void wr64_set_texture_pack(const char* dir) {
    { std::lock_guard<std::mutex> lk(s_tex_mutex); s_tex_pack_dir = (dir ? dir : ""); }
    s_tex_dirty.store(true);
}
void wr64_set_texture_replace(bool enabled) { s_tex_replace_enabled.store(enabled); s_tex_dirty.store(true); }
void wr64_set_texture_dump(bool enabled)    { s_tex_dump_enabled.store(enabled);    s_tex_dirty.store(true); }

// ---------------------------------------------------------------------------
// GraphicsConfig → RT64 UserConfiguration helpers
// ---------------------------------------------------------------------------

static RT64::UserConfiguration::RefreshRate to_rt64_rr(ultramodern::renderer::RefreshRate opt) {
    switch (opt) {
        case ultramodern::renderer::RefreshRate::Original: return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Display:  return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual:   return RT64::UserConfiguration::RefreshRate::Manual;
        default:                                           return RT64::UserConfiguration::RefreshRate::Display;
    }
}

static RT64::UserConfiguration::Antialiasing to_rt64_aa(ultramodern::renderer::Antialiasing opt) {
    switch (opt) {
        case ultramodern::renderer::Antialiasing::MSAA2X: return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X: return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X: return RT64::UserConfiguration::Antialiasing::MSAA8X;
        default:                                          return RT64::UserConfiguration::Antialiasing::None;
    }
}

static RT64::UserConfiguration::InternalColorFormat to_rt64_hpfb(ultramodern::renderer::HighPrecisionFramebuffer opt) {
    switch (opt) {
        case ultramodern::renderer::HighPrecisionFramebuffer::On:   return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto: return RT64::UserConfiguration::InternalColorFormat::Automatic;
        default:                                                     return RT64::UserConfiguration::InternalColorFormat::Standard;
    }
}

// Apply launcher GraphicsConfig to RT64's UserConfiguration.
// NOTE: aspectRatio is intentionally not forwarded here — WR64 permanently
// uses AspectRatio::Expand (set at construction) per the scene-aspect design.
static void apply_graphics_config_to_rt64(RT64::Application* app,
                                           const ultramodern::renderer::GraphicsConfig& cfg) {
    // Resolution / downsampling
    switch (cfg.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            app->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            app->userConfig.resolutionMultiplier = std::max(cfg.ds_option, 1);
            app->userConfig.downsampleMultiplier = std::max(cfg.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original2x:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            app->userConfig.resolutionMultiplier = 2.0f * std::max(cfg.ds_option, 1);
            app->userConfig.downsampleMultiplier = std::max(cfg.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original3x:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            app->userConfig.resolutionMultiplier = 3.0f * std::max(cfg.ds_option, 1);
            app->userConfig.downsampleMultiplier = std::max(cfg.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original4x:
            app->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            app->userConfig.resolutionMultiplier = 4.0f * std::max(cfg.ds_option, 1);
            app->userConfig.downsampleMultiplier = std::max(cfg.ds_option, 1);
            break;
    }

    // Refresh rate / framerate cap
    app->userConfig.refreshRate = to_rt64_rr(cfg.rr_option);
    app->userConfig.refreshRateTarget = cfg.rr_manual_value;

    // Anti-aliasing
    app->userConfig.antialiasing = to_rt64_aa(cfg.msaa_option);

    // High-precision framebuffer
    app->userConfig.internalColorFormat = to_rt64_hpfb(cfg.hpfb_option);

    app->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
}

// ---------------------------------------------------------------------------
// Culling-bounds bisection harness (RE tooling, env-driven; see
// scripts/bisect_culling.py). The game culls objects/waves against its
// original view rect; these hooks locate the responsible RDRAM variables:
//   WR64_POKE_SCAN=1        scan RDRAM for view-rect-shaped s16 pairs at
//                           ~5s and write them to poke_candidates.txt
//   WR64_POKE_FILE=<path>   candidate list to poke
//   WR64_POKE_RANGE=lo:hi   candidate index range [lo,hi) to poke (widen)
//                           continuously every update
// ---------------------------------------------------------------------------
namespace {

struct PokeCandidate {
    uint32_t addr;    // guest address (s16 pair or f32 quad)
    int kind;         // 0 = s16 view-rect pair, 1 = f32 clip-plane quad
    uint16_t a, b;    // kind 0: original values
    uint32_t w[4];    // kind 1: original f32 bit patterns [-x,+x,-y,+y]
};
static std::vector<PokeCandidate> s_poke_list;
static bool s_poke_loaded = false;
static size_t s_poke_lo = 0, s_poke_hi = 0;
// FOV widening factor for kind-2 pokes (WR64_POKE_FOV carries the factor;
// "1" or unparsable = default 1.3).
static float s_fov_scale = 1.3f;

static uint16_t rd16g(uint8_t* rdram, uint32_t g) {
    return *(uint16_t*)(rdram + ((g ^ 2) - 0x80000000u));
}
static void wr16g(uint8_t* rdram, uint32_t g, uint16_t v) {
    *(uint16_t*)(rdram + ((g ^ 2) - 0x80000000u)) = v;
}

static bool is_low_bound(uint16_t v) { return v == 8 || v == 20; }
static bool is_high_bound(uint16_t v) {
    return v == 310 || v == 311 || v == 312 || v == 217 || v == 218 || v == 219 || v == 224;
}
static uint16_t widen_high(uint16_t v) { return (v >= 300) ? 320 : 240; }

static uint32_t rd32g(uint8_t* rdram, uint32_t g) {
    return *(uint32_t*)(rdram + (g - 0x80000000u));
}
static void wr32g(uint8_t* rdram, uint32_t g, uint32_t v) {
    *(uint32_t*)(rdram + (g - 0x80000000u)) = v;
}
static float bits_to_f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static uint32_t f_to_bits(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

static void poke_scan(uint8_t* rdram, int mode) {
    FILE* f = fopen("poke_candidates.txt", "w");
    if (!f) return;
    int count = 0;
    if (mode == 1) {
        // s16 view-rect pairs.
        for (uint32_t g = 0x80000010; g < 0x807FFFF0 && count < 4096; g += 2) {
            uint16_t a = rd16g(rdram, g), b = rd16g(rdram, g + 2);
            if (is_low_bound(a) && is_high_bound(b)) {
                fprintf(f, "S 0x%08X %u %u\n", g, a, b);
                count++;
            }
        }
    } else if (mode == 2) {
        // f32 clip-plane quads [-x,+x,-y,+y] (PW64-style camera frustum).
        for (uint32_t g = 0x80000010; g < 0x807FFFE0 && count < 4096; g += 4) {
            float x0 = bits_to_f(rd32g(rdram, g));
            float x1 = bits_to_f(rd32g(rdram, g + 4));
            float y0 = bits_to_f(rd32g(rdram, g + 8));
            float y1 = bits_to_f(rd32g(rdram, g + 12));
            if (!(x0 < 0 && y0 < 0)) continue;
            if (x1 != -x0 || y1 != -y0) continue;
            float ax = x1, ay = y1;
            if (ax < 0.15f || ax > 1.6f || ay < 0.08f || ay > 1.2f) continue;
            if (ax <= ay) continue; // x half-extent should exceed y (4:3-ish)
            fprintf(f, "F 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X\n", g,
                rd32g(rdram, g), rd32g(rdram, g + 4), rd32g(rdram, g + 8), rd32g(rdram, g + 12));
            count++;
        }
    } else if (mode == 3) {
        // aspect-ratio constants (1.3333f = 0x3FAAAAAB — the value the
        // community widescreen GameShark codes patch). Poked as single floats.
        for (uint32_t g = 0x80000010; g < 0x807FFFF0 && count < 4096; g += 4) {
            uint32_t w = rd32g(rdram, g);
            if (w == 0x3FAAAAABu || w == 0x3FAAAAAAu) {
                fprintf(f, "A 0x%08X 0x%08X\n", g, w);
                count++;
            }
        }
    } else if (mode == 5) {
        // Vp (RSP viewport) structs mapping the 3D world into the inner view
        // rect: s16 vscale[4] then s16 vtrans[4], components in 10.2 fixed.
        // Inner rect (8,20)-(310,218) = 302x198 centered (159,119):
        // vscale=(604,396,z,0), vtrans=(636,476,z,0). The viewport — not the
        // scissor — is why widened-scissor frames still show empty margins:
        // geometry can't land outside the viewport rect. No G_MOVEMEM
        // viewport appears at the top level of gameplay DLs (set from
        // branched sub-DLs), so the structs get poked directly.
        for (uint32_t g = 0x80000010; g < 0x807FFFE0 && count < 4096; g += 2) {
            // Exact expected signature.
            if (rd16g(rdram, g) == 604 && rd16g(rdram, g + 2) == 396 &&
                rd16g(rdram, g + 6) == 0 &&
                rd16g(rdram, g + 8) == 636 && rd16g(rdram, g + 10) == 476 &&
                rd16g(rdram, g + 14) == 0) {
                fprintf(f, "V 0x%08X %d %d\n", g,
                    (int16_t)rd16g(rdram, g + 4), (int16_t)rd16g(rdram, g + 12));
                count++;
            }
            // Relaxed probes, in case the real struct differs from the guess:
            // any vscale=(604,396) pair, or any vtrans=(636,476) pair, dumped
            // with 8 surrounding shorts for manual identification.
            else if ((rd16g(rdram, g) == 604 && rd16g(rdram, g + 2) == 396) ||
                     (rd16g(rdram, g) == 636 && rd16g(rdram, g + 2) == 476)) {
                fprintf(f, "v 0x%08X ", g);
                for (int k = -2; k < 6; k++)
                    fprintf(f, "%d ", (int16_t)rd16g(rdram, g + k * 2));
                fprintf(f, "\n");
                count++;
            }
        }
    } else if (mode == 6) {
        // View-rect 4-TUPLES: the shore/banner/fence/buoy screen-space clip
        // compares against the full view rect, which mode 1 (adjacent pairs)
        // could not find. Match {x0,y0,x1,y1} or {x0,x1,y0,y1} orderings in
        // both s16[4] and s32[4] layouts, with the known rect flavors:
        // x0=8, y0 in {12,20}, x1 in {310,311,312}, y1 in {218,219,224,228}.
        auto is_x1 = [](int v) { return v >= 310 && v <= 312; };
        auto is_y1 = [](int v) { return v == 218 || v == 219 || v == 224 || v == 228; };
        auto is_y0 = [](int v) { return v == 12 || v == 20; };
        for (uint32_t g = 0x80000010; g < 0x807FFFE0 && count < 4096; g += 2) {
            int a = (int16_t)rd16g(rdram, g), b = (int16_t)rd16g(rdram, g + 2);
            int c = (int16_t)rd16g(rdram, g + 4), d = (int16_t)rd16g(rdram, g + 6);
            if (a == 8 && ((is_y0(b) && is_x1(c) && is_y1(d)) ||
                           (is_x1(b) && is_y0(c) && is_y1(d)))) {
                fprintf(f, "R16 0x%08X %d %d %d %d\n", g, a, b, c, d);
                count++;
            }
        }
        for (uint32_t g = 0x80000010; g < 0x807FFFD0 && count < 4096; g += 4) {
            int32_t a = (int32_t)rd32g(rdram, g), b = (int32_t)rd32g(rdram, g + 4);
            int32_t c = (int32_t)rd32g(rdram, g + 8), d = (int32_t)rd32g(rdram, g + 12);
            if (a == 8 && ((is_y0(b) && is_x1(c) && is_y1(d)) ||
                           (is_x1(b) && is_y0(c) && is_y1(d)))) {
                fprintf(f, "R32 0x%08X %d %d %d %d\n", g, (int)a, (int)b, (int)c, (int)d);
                count++;
            }
        }
        // Float rect variants (8.0f, 310.0f, ...).
        for (uint32_t g = 0x80000010; g < 0x807FFFD0 && count < 4096; g += 4) {
            float a = bits_to_f(rd32g(rdram, g)), b = bits_to_f(rd32g(rdram, g + 4));
            float c = bits_to_f(rd32g(rdram, g + 8)), d = bits_to_f(rd32g(rdram, g + 12));
            if (a == 8.0f && ((is_y0((int)b) && is_x1((int)c) && is_y1((int)d) && b == (int)b && c == (int)c && d == (int)d) ||
                              (is_x1((int)b) && is_y0((int)c) && is_y1((int)d) && b == (int)b && c == (int)c && d == (int)d))) {
                fprintf(f, "RF 0x%08X %.0f %.0f %.0f %.0f\n", g, a, b, c, d);
                count++;
            }
        }
    } else {
        // mode 4: camera FOV values. RT64 telemetry proved the live projection
        // is guPerspective(fovy=45deg, 4:3): m11 == cot(22.5deg). The camera
        // struct holds fovy as a float — scan for 45.0f (0x42340000) and the
        // demo cam's ~75.0f (0x42960000). Poked as single floats (x1.3).
        for (uint32_t g = 0x80000010; g < 0x807FFFF0 && count < 4096; g += 4) {
            uint32_t w = rd32g(rdram, g);
            if (w == 0x42340000u || w == 0x42960000u) {
                fprintf(f, "A 0x%08X 0x%08X\n", g, w);
                count++;
            }
        }
    }
    fclose(f);
    fprintf(stderr, "[POKE] scan mode %d complete: %d candidates\n", mode, count);
}

static void poke_load() {
    s_poke_loaded = true;
    const char* file = std::getenv("WR64_POKE_FILE");
    const char* range = std::getenv("WR64_POKE_RANGE");
    if (!file || !range) return;
    FILE* f = fopen(file, "r");
    if (!f) return;
    char kind = 0;
    unsigned int addr = 0, a = 0, b = 0;
    unsigned int w0, w1, w2, w3;
    while (fscanf(f, " %c", &kind) == 1) {
        PokeCandidate c{};
        if (kind == 'S' && fscanf(f, "%x %u %u", &addr, &a, &b) == 3) {
            c.addr = addr; c.kind = 0; c.a = (uint16_t)a; c.b = (uint16_t)b;
            s_poke_list.push_back(c);
        } else if (kind == 'F' && fscanf(f, "%x %x %x %x %x", &addr, &w0, &w1, &w2, &w3) == 5) {
            c.addr = addr; c.kind = 1;
            c.w[0] = w0; c.w[1] = w1; c.w[2] = w2; c.w[3] = w3;
            s_poke_list.push_back(c);
        } else if (kind == 'A' && fscanf(f, "%x %x", &addr, &w0) == 2) {
            c.addr = addr; c.kind = 2; c.w[0] = w0;
            s_poke_list.push_back(c);
        } else {
            break;
        }
    }
    fclose(f);
    unsigned long lo = 0, hi = 0;
    if (sscanf(range, "%lu:%lu", &lo, &hi) == 2) {
        s_poke_lo = lo;
        s_poke_hi = (hi > s_poke_list.size()) ? s_poke_list.size() : hi;
    }
    fprintf(stderr, "[POKE] loaded %zu candidates, poking [%zu,%zu)\n",
        s_poke_list.size(), s_poke_lo, s_poke_hi);
}

// Targeted frustum poke (WR64_POKE_FRUSTUM=1): the camera/frustum parameter
// structs live in a static array at 0x801D7B70 (stride 0x24; entry active when
// word +0x00 != 0). func_800B4ABC feeds guFrustum from fields +0x04 (int, left
// source), +0x1C (f32, right source), +0x14 (f32, top source). Scale them 1.3x
// to test whether widening the frustum fills the culled margins.
static void poke_frustum(uint8_t* rdram) {
    constexpr uint32_t BASE = 0x801D7B70;
    constexpr uint32_t STRIDE = 0x24;
    constexpr float SCALE = 1.3f;
    static uint32_t last_written[8][3] = {};
    // Periodic state dump (every ~300 calls), including when NOTHING is
    // active: an earlier run logged only on the first active entry, so an
    // all-inactive race was indistinguishable from a poke that worked.
    static uint32_t calls = 0;
    if ((++calls % 300) == 1) {
        for (int i = 0; i < 8; i++) {
            uint32_t entry = BASE + i * STRIDE;
            if (rd32g(rdram, entry) == 0) continue;
            fprintf(stderr, "[FRUSTUM] entry %d fields:", i);
            for (int k = 0; k < 9; k++)
                fprintf(stderr, " +%02X=0x%08X", k * 4, rd32g(rdram, entry + k * 4));
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "[FRUSTUM] scan tick %u done\n", calls);
    }
    for (int i = 0; i < 8; i++) {
        uint32_t entry = BASE + i * STRIDE;
        if (rd32g(rdram, entry) == 0) continue;

        uint32_t v_int = rd32g(rdram, entry + 0x04);
        uint32_t v_r = rd32g(rdram, entry + 0x1C);
        uint32_t v_t = rd32g(rdram, entry + 0x14);

        if (v_int != last_written[i][0]) {
            int32_t scaled = (int32_t)((int32_t)v_int * SCALE);
            wr32g(rdram, entry + 0x04, (uint32_t)scaled);
            last_written[i][0] = (uint32_t)scaled;
        }
        if (v_r != last_written[i][1]) {
            uint32_t scaled = f_to_bits(bits_to_f(v_r) * SCALE);
            wr32g(rdram, entry + 0x1C, scaled);
            last_written[i][1] = scaled;
        }
        if (v_t != last_written[i][2]) {
            uint32_t scaled = f_to_bits(bits_to_f(v_t) * SCALE);
            wr32g(rdram, entry + 0x14, scaled);
            last_written[i][2] = scaled;
        }
    }
}

static void poke_apply(uint8_t* rdram) {
    for (size_t i = s_poke_lo; i < s_poke_hi; i++) {
        const PokeCandidate& c = s_poke_list[i];
        // Only poke while the location still holds the original values (heap
        // data may have been reused; the game may also rewrite per frame, in
        // which case this re-fires every update).
        if (c.kind == 0) {
            if (rd16g(rdram, c.addr) == c.a && rd16g(rdram, c.addr + 2) == c.b) {
                wr16g(rdram, c.addr, 0);
                wr16g(rdram, c.addr + 2, widen_high(c.b));
            }
        } else if (c.kind == 1) {
            if (rd32g(rdram, c.addr) == c.w[0] && rd32g(rdram, c.addr + 4) == c.w[1] &&
                rd32g(rdram, c.addr + 8) == c.w[2] && rd32g(rdram, c.addr + 12) == c.w[3]) {
                // Widen the frustum dramatically (1.5x) so hits are obvious.
                for (int k = 0; k < 4; k++) {
                    wr32g(rdram, c.addr + k * 4, f_to_bits(bits_to_f(c.w[k]) * 1.5f));
                }
            }
        } else {
            // FOV/aspect value: widen by the configured factor.
            if (rd32g(rdram, c.addr) == c.w[0]) {
                wr32g(rdram, c.addr, f_to_bits(bits_to_f(c.w[0]) * s_fov_scale));
                // Log each live write once — the set of addresses the game
                // actively re-reads is the candidate shortlist.
                static std::vector<uint32_t> logged_addrs;
                bool seen = false;
                for (uint32_t a : logged_addrs) { if (a == c.addr) { seen = true; break; } }
                if (!seen && logged_addrs.size() < 64) {
                    logged_addrs.push_back(c.addr);
                    fprintf(stderr, "[FOV] live poke at 0x%08X (%.1f -> %.1f)\n",
                        c.addr, bits_to_f(c.w[0]), bits_to_f(c.w[0]) * s_fov_scale);
                }
            }
        }
    }
}

} // namespace
// Whether developer tooling is enabled; gates ALL RT64 debug shortcuts.
// (RT64 itself only gates F1/Inspector — F2/F3/F4 toggle ray tracing, the raw
// RDRAM framebuffer view, and texture replacements even in normal play, which
// surprises players. We only forward events when dev mode is on.)
static std::atomic<bool> s_dev_mode{false};
// Game frames (display lists) submitted since last consumption, for FPS display.
static std::atomic<uint32_t> s_frame_count{0};

// ---------------------------------------------------------------------------
// RT64 RendererContext subclass
// ---------------------------------------------------------------------------

namespace wr64 {

class RT64Context : public ultramodern::renderer::RendererContext {
public:
    RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
#ifdef HAS_RECOMPUI
        // Register RmlUi render hooks with RT64 before the application is set up.
        // The hooks fire during RT64's present pipeline to draw the launcher UI.
        recompui::set_render_hooks();
#endif

        // Populate the RT64 core struct from the emulated hardware state.
        RT64::Application::Core core{};

        // The RDRAM pointer is the base of the emulated N64 memory.
        core.RDRAM = rdram;

        // Use the static dummy header — real ROM header bytes are not needed for HLE.
        core.HEADER = s_dummy_rom_header;

        // DMEM/IMEM must be non-null; RT64 may reference them during state init.
        core.DMEM = s_DMEM;
        core.IMEM = s_IMEM;

        // Wire up a no-op interrupt callback. The ultramodern runtime owns
        // interrupt delivery; RT64 does not need to trigger them directly.
        core.checkInterrupts = dummy_check_interrupts;

        // RDP / MI registers — provide real storage so RT64 can read/write safely.
        core.MI_INTR_REG      = &s_MI_INTR_REG;
        core.DPC_START_REG    = &s_DPC_START_REG;
        core.DPC_END_REG      = &s_DPC_END_REG;
        core.DPC_CURRENT_REG  = &s_DPC_CURRENT_REG;
        core.DPC_STATUS_REG   = &s_DPC_STATUS_REG;
        core.DPC_CLOCK_REG    = &s_DPC_CLOCK_REG;
        core.DPC_BUFBUSY_REG  = &s_DPC_BUFBUSY_REG;
        core.DPC_PIPEBUSY_REG = &s_DPC_PIPEBUSY_REG;
        core.DPC_TMEM_REG     = &s_DPC_TMEM_REG;

        // VI register pointers: RT64 reads these from RDRAM to determine framebuffer.
        // The ultramodern runtime sets up VI registers at known addresses.
        // We obtain the VI registers from the ultramodern runtime.
        auto* vi_regs = ultramodern::renderer::get_vi_regs();
        core.VI_STATUS_REG          = &vi_regs->VI_STATUS_REG;
        core.VI_ORIGIN_REG          = &vi_regs->VI_ORIGIN_REG;
        core.VI_WIDTH_REG           = &vi_regs->VI_WIDTH_REG;
        core.VI_INTR_REG            = &vi_regs->VI_INTR_REG;
        core.VI_V_CURRENT_LINE_REG  = &vi_regs->VI_V_CURRENT_LINE_REG;
        core.VI_TIMING_REG          = &vi_regs->VI_TIMING_REG;
        core.VI_V_SYNC_REG          = &vi_regs->VI_V_SYNC_REG;
        core.VI_H_SYNC_REG          = &vi_regs->VI_H_SYNC_REG;
        core.VI_LEAP_REG            = &vi_regs->VI_LEAP_REG;
        core.VI_H_START_REG         = &vi_regs->VI_H_START_REG;
        core.VI_V_START_REG         = &vi_regs->VI_V_START_REG;
        core.VI_V_BURST_REG         = &vi_regs->VI_V_BURST_REG;
        core.VI_X_SCALE_REG         = &vi_regs->VI_X_SCALE_REG;
        core.VI_Y_SCALE_REG         = &vi_regs->VI_Y_SCALE_REG;

        // Set up the SDL window for RT64.
#if defined(_WIN32)
        core.window = window_handle.window;
#elif defined(__APPLE__)
        core.window.window = window_handle.window;
        core.window.view   = window_handle.view;
#else
        // Linux / Android: WindowHandle IS SDL_Window*
        core.window = window_handle;
#endif

        // Configure the RT64 application.
        RT64::ApplicationConfiguration app_config{};
        app_config.appId = "waverace64";
        // Disable config file I/O — we manage settings ourselves.
        app_config.useConfigurationFile = false;

        // Create the RT64 application.
        app_ = std::make_unique<RT64::Application>(core, app_config);

        // Apply launcher graphics settings (framerate cap, resolution, MSAA, etc.).
        apply_graphics_config_to_rt64(app_.get(), ultramodern::renderer::get_graphics_config());

        // Enable developer/debug mode if requested.
        app_->userConfig.developerMode = developer_mode;

        // Initialize WR64 game-settings statics. The launcher config load
        // callback (apply_wr64) has already run by construction time and set
        // s_show_borders from the saved preference; WR64_BORDERS env overrides.
        {
            const char* b = std::getenv("WR64_BORDERS");
            if (b && b[0] == '1') s_show_borders = true;
            else if (b && b[0] == '0') s_show_borders = false;

            const char* wg = std::getenv("WR64_WAVEGRID");
            if (wg) {
                unsigned r = 0, c = 0;
                if (sscanf(wg, "%ux%u", &r, &c) == 2 && r >= 4 && c >= 4) {
                    s_wavegrid_rows = r > 40 ? 40 : r;
                    s_wavegrid_cols = c > 96 ? 96 : c;
                }
            }
        }

        // Aspect ratio is chosen ONCE here and never flipped at runtime (a
        // runtime UserConfiguration change crashes in-flight queues with
        // discardFBs, or ghosts stale targets without it — see docs/RE-NOTES).
        //   - Border removal (default): Expand — the game rectangle is widened
        //     to fill the window; per-scene menu pillarboxing is done by the
        //     VI-blit crop (send_dl below).
        //   - Show Borders (stock): Original — the game renders its native 4:3
        //     frame with its own black overscan borders, pillarboxed in the
        //     window at correct proportions. Because this is boot-time, toggling
        //     the setting requires an application restart.
        // WR64_WIDESCREEN=0 forces Original regardless (testing).
        const char* ws_env = std::getenv("WR64_WIDESCREEN");
        const bool widescreen = !(ws_env && ws_env[0] == '0');
        if (widescreen && !s_show_borders) {
            app_->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
        } else {
            app_->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Original;
        }

        // WR64_HIGHFPS=1 forces display-rate presentation even if the launcher
        // config is set to Original (backwards-compat env var override).
        const char* highfps_env = std::getenv("WR64_HIGHFPS");
        if (highfps_env && highfps_env[0] == '1') {
            app_->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Display;
        }

        // Attempt setup.
        auto result = app_->setup(0);

        switch (result) {
            case RT64::Application::SetupResult::Success:
                setup_result = ultramodern::renderer::SetupResult::Success;
                break;
            case RT64::Application::SetupResult::DynamicLibrariesNotFound:
                setup_result = ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
                break;
            case RT64::Application::SetupResult::InvalidGraphicsAPI:
                setup_result = ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
                break;
            case RT64::Application::SetupResult::GraphicsAPINotFound:
                setup_result = ultramodern::renderer::SetupResult::GraphicsAPINotFound;
                break;
            case RT64::Application::SetupResult::GraphicsDeviceNotFound:
                setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
                break;
        }

        if (result != RT64::Application::SetupResult::Success) {
            app_.reset();
            return;
        }

        // Map the API that RT64 actually chose.
        switch (app_->chosenGraphicsAPI) {
            case RT64::UserConfiguration::GraphicsAPI::D3D12:
                chosen_api = ultramodern::renderer::GraphicsApi::D3D12;
                break;
            case RT64::UserConfiguration::GraphicsAPI::Vulkan:
                chosen_api = ultramodern::renderer::GraphicsApi::Vulkan;
                break;
            case RT64::UserConfiguration::GraphicsAPI::Metal:
                chosen_api = ultramodern::renderer::GraphicsApi::Metal;
                break;
            default:
                chosen_api = ultramodern::renderer::GraphicsApi::Vulkan;
                break;
        }

        // ---- HD texture packs (replacement) + texture dumping ----
        // RT64's texture-replacement system is driven by the launcher "Textures"
        // tab (wr64_set_texture_* below) and, as startup overrides, the env vars
        // WR64_TEXPACK / WR64_TEXDUMP. Env vars win at launch; runtime launcher
        // changes are reconciled on the gfx thread (apply_texture_state_gfx,
        // called from update_screen). The actual RT64 mutation always happens on
        // the gfx thread to stay safe against send_dl/update_screen.
        {
            const char* pack = std::getenv("WR64_TEXPACK");
            if (pack && pack[0] != '\0' && std::strcmp(pack, "0") != 0) {
                std::lock_guard<std::mutex> lk(s_tex_mutex);
                s_tex_pack_dir = (std::strcmp(pack, "1") == 0) ? "textures" : pack;
                s_tex_replace_enabled.store(true);
            }
            const char* dump = std::getenv("WR64_TEXDUMP");
            if (dump && dump[0] != '\0' && std::strcmp(dump, "0") != 0) {
                if (std::strcmp(dump, "1") != 0) {  // custom dir (power-user override)
                    std::lock_guard<std::mutex> lk(s_tex_mutex);
                    s_tex_dump_dir = dump;
                }
                s_tex_dump_enabled.store(true);
            }
            s_tex_dirty.store(true);  // first update_screen applies it
        }

        printf("[WR64-RT64] Renderer context created (result=%d, api=%d)\n",
               static_cast<int>(result), static_cast<int>(chosen_api));
        s_app.store(app_.get());
    }

    ~RT64Context() override {
        s_app.store(nullptr);
        if (app_) {
            app_->end();
        }
    }

    bool valid() override {
        return setup_result == ultramodern::renderer::SetupResult::Success && app_ != nullptr;
    }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        if (!app_ || old_config == new_config) return false;

        if (new_config.wm_option != old_config.wm_option) {
            app_->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
        }

        apply_graphics_config_to_rt64(app_.get(), new_config);

        // Re-apply the WR64 aspect ratio override so a launcher graphics-config
        // apply can't reset it. MUST mirror the constructor's decision: Expand
        // for border removal (default), Original for Show Borders (stock 4:3
        // with the game's own black borders). This callback fires on the
        // startup config apply too, so forcing Expand unconditionally here was
        // silently reverting Show Borders back to the widened look.
        const char* ws_env = std::getenv("WR64_WIDESCREEN");
        const bool widescreen = !(ws_env && ws_env[0] == '0');
        app_->userConfig.aspectRatio = (widescreen && !s_show_borders)
            ? RT64::UserConfiguration::AspectRatio::Expand
            : RT64::UserConfiguration::AspectRatio::Original;

        bool res_changed  = new_config.res_option  != old_config.res_option;
        bool ar_changed   = new_config.ar_option   != old_config.ar_option;
        bool ds_changed   = new_config.ds_option   != old_config.ds_option;
        bool msaa_changed = new_config.msaa_option != old_config.msaa_option;
        app_->updateUserConfig(res_changed || ar_changed || ds_changed || msaa_changed);

        if (msaa_changed) {
            app_->updateMultisampling();
        }
        return true;
    }

    void enable_instant_present() override {
        if (!app_) return;
        // Enable present-early mode for minimal latency (matches reference).
        app_->enhancementConfig.presentation.mode =
            RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        app_->updateEnhancementConfig();
    }

    void send_dl(const OSTask* task) override {
        if (!app_) return;

        // task->t.ucode / ucode_data / data_ptr are PTR(u64) = int32_t holding
        // N64 KSEG0 virtual addresses (e.g. 0x80XXXXXX).
        // RT64 expects physical byte offsets into the RDRAM buffer.
        // Masking with 0x3FFFFFF strips the KSEG0/KSEG1 high bits and gives
        // the physical address within the N64's 64MB address space.
        uint32_t ucode_phys      = static_cast<uint32_t>(task->t.ucode)      & 0x3FFFFFFu;
        uint32_t ucode_data_phys = static_cast<uint32_t>(task->t.ucode_data) & 0x3FFFFFFu;
        uint32_t dl_start_phys   = static_cast<uint32_t>(task->t.data_ptr)   & 0x3FFFFFFu;

        // Border removal (DEFAULT ON since 2026-07-17; WR64_BORDERS=1
        // restores the stock look): the game scissors rendering to
        // ~(8,20)-(310,218), drawing black CRT-overscan borders inside its
        // own framebuffer. This block rewrites/widens the scissors (top-level
        // words here, sub-DL scissors via the rt64 fork hooks), widens the
        // fullscreen tint, enlarges the wave grid, and drives the per-scene
        // 4:3 menu presentation. Works together with the shipped 47.75-deg
        // fovy instruction patches (recomp/waverace64.toml). Known remaining
        // seams at extreme aspect ratios: shore-strip geometry (game
        // CPU-clips it to its view rect) and the foam framebuffer effect
        // (cannot extend past the original fb region). History and RE trail:
        // docs/RE-NOTES.md.
        if (!s_show_borders) {
            // Border-removal mode (Expand aspect). Scene classification drives
            // the per-scene menu 4:3 crop plus the widescreen widening hooks.
            {
                uint8_t* rdram = app_->core.RDRAM;
                const bool border_removal = true;

                // Pass 1: gameplay detection. Gameplay frames draw the
                // fullscreen tint texrect (0xE44D8368); menus don't. Menu
                // frames keep their original scissor so 3D props the game
                // parks off-screen stay clipped (RT64's relaxed similarity
                // check keeps menu 2D centered without the rewrite).
                // Scene classification (drives the scissor rewrite and the
                // per-scene aspect): gameplay and cinematic flybys get the
                // widescreen treatment, menus stay 4:3. Signals, measured from
                // real DLs: gameplay draws the fullscreen tint texrect
                // (0xE44D8368); cinematic flybys draw no tint and at most a
                // couple of texrects (the logo); menus draw no tint but a
                // stack of panel texrects.
                // Scene signals, measured from real DLs:
                // - The game's WORLD projection always loads via segment 3
                //   (G_MTX w1 = 0x03xxxxxx). Races, demos, flybys, and menus
                //   with live world backdrops (e.g. difficulty select) all
                //   carry it; pure 2D menus (watercraft select) draw their
                //   parked world under a dedicated fovy=50 camera instead.
                // - The fullscreen tint texrect (0xE44D8368) marks racing
                //   frames (used by the widening rewrite below).
                bool has_tint = false;
                {
                    uint32_t scan = dl_start_phys;
                    for (int i = 0; i < 0x4000 && scan < 0x7FFFF8u; i++, scan += 8) {
                        uint32_t w0s = *(uint32_t*)(rdram + scan);
                        uint8_t ops = w0s >> 24;
                        if (ops == 0xB8) break; // G_ENDDL
                        if (ops == 0x06 && ((w0s >> 16) & 0xFF) == 0x01) break; // G_DL branch
                        if (ops == 0xE4 && w0s == 0xE44D8368u) has_tint = true;
                    }
                }
                // World detection comes from RT64's RSP itself: counters of
                // segment-3 projections geometry was actually drawn under.
                // Raw DL scans false-positived on stale/branched-over buffer
                // content, and even real seg-3 *loads* happen in menus — the
                // menus also render a parked world (the prop jetskis) behind
                // their backdrop, but under a dedicated fovy=50 camera which
                // the RSP patch counts separately as "menu world". Live
                // cameras (races 45, demos 75, ...) count as world. Counters
                // cover tasks processed up to the previous send_dl — one task
                // of latency, absorbed by the Schmitt trigger below.
                bool has_world_proj = false;
                bool has_menu_world = false;
                {
                    static uint32_t last_world_loads = 0;
                    static uint32_t last_menu_world_loads = 0;
                    const uint32_t world_loads = rt64_wr64_world_proj_loads();
                    const uint32_t menu_world_loads = rt64_wr64_menu_world_proj_loads();
                    has_world_proj = (world_loads != last_world_loads);
                    has_menu_world = (menu_world_loads != last_menu_world_loads);
                    last_world_loads = world_loads;
                    last_menu_world_loads = menu_world_loads;
                }
                // Schmitt-trigger smoothing. Only positive evidence moves the
                // score: a drawn live-world camera pushes toward wide, a drawn
                // menu-world camera pushes toward menu, and blank/fade frames
                // (neither) hold the current state instead of drifting —
                // loading fades must not flip the aspect.
                static int menu_score = 0;
                if (has_world_proj) {
                    menu_score -= 15;
                }
                else if (has_menu_world) {
                    menu_score += 15;
                }
                if (menu_score < 0) menu_score = 0;
                if (menu_score > 120) menu_score = 120;
                static bool menu_state = false;
                if (!menu_state && menu_score >= 60) menu_state = true;
                if (menu_state && menu_score <= 30) menu_state = false;
                const bool is_gameplay = !menu_state;
                static const char* dbg_env = std::getenv("WR64_SCENE_DEBUG");
                if (dbg_env && dbg_env[0] == '1') {
                    static uint32_t dbg_frames = 0;
                    if ((++dbg_frames % 20) == 0) {
                        fprintf(stderr, "[SCENE] world=%d menuworld=%d tint=%d score=%d -> %s\n",
                            (int)has_world_proj, (int)has_menu_world, (int)has_tint,
                            menu_score, is_gameplay ? "wide" : "menu");
                    }
                }

                // Menus manage only the original 4:3 region (parked 3D props
                // and un-cleared framebuffer areas sit outside it), so
                // widescreen expansion must not apply to them: switch RT64's
                // aspect mode per scene type. Gameplay gets Expand (unless
                // WR64_WIDESCREEN=0), menus get Original (pillarboxed).
                {
                    static const char* ws_env2 = std::getenv("WR64_WIDESCREEN");
                    static const char* sa_env = std::getenv("WR64_SCENE_ASPECT");
                    const bool widescreen_enabled = !(ws_env2 && ws_env2[0] == '0');
                    const bool scene_aspect_enabled = !(sa_env && sa_env[0] == '0');
                    const int wanted = is_gameplay ? 1 : 0;
                    if (widescreen_enabled && scene_aspect_enabled && wanted != s_crop_requested) {
                        s_crop_requested = wanted;
                        // Presentation-level pillarbox: rendering stays wide
                        // (Expand) at all times; only the final blit's scissor
                        // changes. Runtime UserConfiguration flips are not
                        // viable — with the framebuffer discard they crash the
                        // in-flight queues, without it the differently-sized
                        // stale targets ghost through the menu.
                        rt64_wr64_set_present_crop43(wanted == 0 ? 1 : 0);
                    }
                }

                // Widen inner-rect scissors at RT64 processing time too (all
                // sub-DLs, segment-resolved) — the top-level word rewrite
                // below misses the wave-mesh/water pass, whose scissor is set
                // from a branched sub-DL and kept the water clipped to the
                // inner rect. Gameplay frames only, same rule as the rewrite.
                // WR64_SCISSOR_MASK (hex bitmask over per-frame matching
                // scissor indices) selects WHICH matching scissors get
                // widened at RDP STATE level. Default 0x6: indices 1-2 (the
                // wave-mesh passes) widen; index 0 must NOT be widened here —
                // its rect drives RT64's fbPair/projection aspect
                // classification and widening it collapses the world into an
                // unstretched 4:3 band (user-confirmed regression). The
                // per-CALL clipping that index 0 used to cause is handled
                // separately at GPU-scissor conversion time in the rt64 fork
                // (rt64_framebuffer_renderer.cpp, gated by wide_world).
                {
                    static uint32_t scissor_mask = []() {
                        const char* m = std::getenv("WR64_SCISSOR_MASK");
                        return m ? (uint32_t)strtoul(m, nullptr, 16) : 0x6u;
                    }();
                    static bool mask_sent = false;
                    if (!mask_sent) {
                        mask_sent = true;
                        rt64_wr64_set_scissor_widen_mask(scissor_mask);
                    }
                    // Widen only during gameplay AND only when border removal
                    // is active. In borders-on (stock) mode all widening is off
                    // so the game presents its native frame with its own black
                    // borders; menus in either mode get no widening.
                    const bool widen = border_removal && is_gameplay;
                    rt64_wr64_set_scissor_widen(widen ? 1 : 0);
                    rt64_wr64_set_viewport_widen(widen ? 1 : 0);
                    // Wave-grid enlargement: the detail-water mesh is built
                    // per frame as a rows x cols camera-facing grid whose
                    // dimensions live in a static config block at 0x800DA8B4
                    // ({flag, rows=19, cols=35, Vp...} — found via decomp
                    // func_8008FB74, the wave-mesh DL builder, reading it
                    // every frame; docs/RE-NOTES.md). Stock 19x35 covers only
                    // the original 4:3 view; enlarge so detailed water fills
                    // widescreen. 23x55 verified stable at full frame rate
                    // (27x63 also fine). WR64_WAVEGRID=RxC overrides. In
                    // borders-on mode force stock 19x35 so the static config
                    // block doesn't keep the enlarged values after a live toggle.
                    if (widen) {
                        wr32g(rdram, 0x800DA8B8u, s_wavegrid_rows);
                        wr32g(rdram, 0x800DA8BCu, s_wavegrid_cols);
                    } else if (!border_removal && is_gameplay) {
                        wr32g(rdram, 0x800DA8B8u, 19u);
                        wr32g(rdram, 0x800DA8BCu, 35u);
                    }
                    // Keep world projections on RT64's wide-viewport path even
                    // while the game's camera-bob shifts its viewport (the
                    // shifted viewport otherwise fails RT64's coverage test
                    // mid-race and the whole scene collapses into the
                    // original-width center band).
                    rt64_wr64_set_wide_world(widen ? 1 : 0);
                    static const char* sd_env = std::getenv("WR64_SCENE_DEBUG");
                    static uint32_t sc_frames = 0;
                    if (sd_env && sd_env[0] == '1' && (++sc_frames % 200) == 0) {
                        fprintf(stderr, "[SCISSOR] inner-rect matches last frame: %u\n",
                            rt64_wr64_scissor_match_count());
                    }
                }

                uint32_t addr = dl_start_phys;
                for (int i = 0; border_removal && is_gameplay && i < 0x4000 && addr < 0x7FFFF8u; i++, addr += 8) {
                    uint32_t w0 = *(uint32_t*)(rdram + addr);
                    uint8_t op = w0 >> 24;
                    if (op == 0xB8) { // G_ENDDL (F3DEX)
                        break;
                    }
                    if (op == 0x06 && ((w0 >> 16) & 0xFF) == 0x01) { // G_DL branch: logical end
                        break;
                    }
                    if (op == 0x01) { // G_MTX (F3DEX): params in w0 bits 16-23
                        uint8_t params = (w0 >> 16) & 0xFF;
                        // G_MTX_PROJECTION=0x01 | G_MTX_LOAD=0x02
                        if ((params & 0x03) == 0x03) {
                            uint32_t w1 = *(uint32_t*)(rdram + addr + 4);
                            uint32_t mtx = w1 & 0x00FFFFFFu;
                            // N64 Mtx: 16 s16 integer parts then 16 u16 fractions.
                            auto mtx16 = [&](int idx) -> int16_t {
                                return (int16_t)rd16g(rdram, 0x80000000u + mtx + idx * 2);
                            };
                            bool perspective = (mtx16(15) == 0); // m[3][3]
                            static const char* proj_env = std::getenv("WR64_POKE_PROJ");
                            static int proj_seen = 0;
                            proj_seen++;
                            if (perspective && (proj_seen % 200) == 0) {
                                fprintf(stderr,
                                    "[PROJ] perspective mtx at 0x%08X: m00=%d.%04X m11=%d.%04X m23=%d m33=%d\n",
                                    0x80000000u + mtx,
                                    mtx16(0), rd16g(rdram, 0x80000000u + mtx + 32 + 0),
                                    mtx16(5), rd16g(rdram, 0x80000000u + mtx + 32 + 10),
                                    mtx16(11), mtx16(15));
                            }
                            if (perspective && proj_env && proj_env[0] == '1') {
                                // Scale m00/m11 by 1/1.3 (wider FOV) in 16.16.
                                for (int e : { 0, 5 }) {
                                    int32_t v = ((int32_t)mtx16(e) << 16) |
                                        rd16g(rdram, 0x80000000u + mtx + 32 + e * 2);
                                    v = (int32_t)(v / 1.3f);
                                    wr16g(rdram, 0x80000000u + mtx + e * 2, (uint16_t)((v >> 16) & 0xFFFF));
                                    wr16g(rdram, 0x80000000u + mtx + 32 + e * 2, (uint16_t)(v & 0xFFFF));
                                }
                            }
                        }
                    }
                    // The game draws a fullscreen tint/atmosphere texrect sized
                    // to the inner view rect (9,21)-(310,218) every frame —
                    // the source of the color seam when borders are removed.
                    // Widen it to cover the full framebuffer.
                    if (op == 0xE4) {
                        uint32_t w1 = *(uint32_t*)(rdram + addr + 4);
                        // WR64_TEXRECT_LOG=1: log every texrect seen by this
                        // walk (10.2 coords decoded), to find per-scene tint
                        // variants that the exact-match rewrite below misses.
                        static const char* trlog = std::getenv("WR64_TEXRECT_LOG");
                        if (trlog && trlog[0] == '1') {
                            static int tr_seen = 0;
                            if ((++tr_seen % 20) == 0) {
                                fprintf(stderr,
                                    "[TEXRECT] w0=%08X w1=%08X rect=(%u,%u)-(%u,%u) tile=%u\n",
                                    w0, w1,
                                    (w1 >> 14) & 0x3FF, (w1 >> 2) & 0x3FF,
                                    (w0 >> 14) & 0x3FF, (w0 >> 2) & 0x3FF,
                                    (w1 >> 24) & 0x7);
                            }
                        }
                        if (w0 == 0xE44D8368u && (w1 & 0x00FFFFFFu) == 0x00024054u) {
                            // (9,21)-(310,218) -> (0,0)-(320,240), keep tile bits.
                            // Must reach the widened scissor's lrx/lry EXACTLY:
                            // RT64 only exempts a texrect from 2D aspect
                            // compensation (and lets it stretch across the full
                            // widescreen framebuffer) when rect.lrx >=
                            // scissor.lrx (rt64_framebuffer_renderer.cpp,
                            // coversScissorWidth). At (319,239) it was 1px
                            // short, got pinned to the centered 4:3, and left
                            // the expanded margins untinted.
                            *(uint32_t*)(rdram + addr) = 0xE45003C0u;
                            *(uint32_t*)(rdram + addr + 4) = w1 & 0xFF000000u;
                        }
                    }
                    if (op == 0xED) { // G_SETSCISSOR, coords are 10.2 fixed
                        uint32_t w1 = *(uint32_t*)(rdram + addr + 4);
                        uint32_t x0 = (w0 >> 12) & 0xFFF, y0 = w0 & 0xFFF;
                        uint32_t x1 = (w1 >> 12) & 0xFFF, y1 = w1 & 0xFFF;
                        if (x0 <= 40 && y0 <= 88 && x1 >= 1200 && y1 >= 856) {
                            *(uint32_t*)(rdram + addr) = 0xED000000u;
                            *(uint32_t*)(rdram + addr + 4) = (w1 & 0xFF000000u) | (1280u << 12) | 960u;
                        }
                    }
                    // G_MOVEMEM viewport (F3DEX op 0xBC, index G_MV_VIEWPORT).
                    // The 3D world is mapped into the inner view rect by the
                    // VIEWPORT transform, not the scissor — widening the
                    // scissor alone leaves clear-color margins with no
                    // geometry. WR64_VP_LOG=1 dumps every viewport seen so the
                    // inner-rect signature can be identified and rewritten.
                    // F3DEX gsDma1p encoding: w0 = [cmd 8][index 8][length 16],
                    // G_MV_VIEWPORT = 0x80 -> w0 = 0xBC800010. NOTE: no
                    // viewport command appears at the TOP level of the
                    // gameplay DLs (they are set from branched sub-DLs this
                    // linear walk does not follow) — the viewport widening
                    // below therefore pokes the Vp STRUCTS found by RDRAM
                    // scan instead of rewriting DL commands.
                    if (op == 0xBC && ((w0 >> 16) & 0xFF) == 0x80) {
                        static const char* vplog = std::getenv("WR64_VP_LOG");
                        if (vplog && vplog[0] == '1') {
                            uint32_t w1 = *(uint32_t*)(rdram + addr + 4);
                            uint32_t vp = w1 & 0x00FFFFFFu;
                            static int vp_seen = 0;
                            if ((++vp_seen % 20) == 0) {
                                int16_t v[8];
                                for (int k = 0; k < 8; k++)
                                    v[k] = (int16_t)rd16g(rdram, 0x80000000u + vp + k * 2);
                                fprintf(stderr,
                                    "[VP] at 0x%08X scale=(%d,%d,%d,%d) trans=(%d,%d,%d,%d)\n",
                                    0x80000000u + vp,
                                    v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
                            }
                        }
                    }
                }
            }
        } else {
            // Stock mode (Show Borders): aspect was set to Original at boot, so
            // the game presents its native 4:3 frame with its own black overscan
            // borders. Keep every runtime hook inert. (The aspect itself is
            // boot-time; toggling Show Borders in the launcher needs a restart.)
            rt64_wr64_set_scissor_widen(0);
            rt64_wr64_set_viewport_widen(0);
            rt64_wr64_set_wide_world(0);
            rt64_wr64_set_present_crop43(0);
            s_crop_requested = -1;
            wr32g(app_->core.RDRAM, 0x800DA8B8u, 19u);
            wr32g(app_->core.RDRAM, 0x800DA8BCu, 35u);
        }

        // Reset the RSP state machine before processing each new display list.
        // This prevents prior-frame geometry or matrix state from leaking.
        app_->state->rsp->reset();

        // Tell the HLE interpreter which GBI microcode variant to use.
        // This must be called before processDisplayLists() or hleGBI will be null.
        app_->interpreter->loadUCodeGBI(ucode_phys, ucode_data_phys, true);

        // Process the display list. Pass 0 for dlEndAddress — RT64 will walk
        // the list until it encounters a G_ENDDL command.
        app_->processDisplayLists(
            app_->core.RDRAM,
            dl_start_phys,
            0,    // end address: 0 means walk until G_ENDDL
            true  // HLE mode
        );

        s_frame_count.fetch_add(1, std::memory_order_relaxed);
    }

    void send_dummy_workload(uint32_t /*fb_address*/) override {
        // Called when the VI fires but no RSP task is queued (launcher phase).
        // RT64's render hooks fire during updateScreen(), so nothing needed here.
    }

    void update_screen() override {
        if (app_) {
            // Apply any pending launcher texture-setting changes (gfx thread).
            apply_texture_state_gfx(app_.get());
            // Diagnostics: WR64_FB_DUMP=1 dumps the 320x240 framebuffer to
            // fb_dump.bin / fb_dump2.bin / fb_dump3.bin at ~10s/~30s/~50s, for
            // offline analysis with scripts/measure_borders.py.
            static uint32_t update_count = 0;
            ++update_count;

            // Culling-bounds bisection harness (see scripts/bisect_culling.py).
            {
                const char* scan_env = std::getenv("WR64_POKE_SCAN");
                // Rescan every 600 updates after the first: per-scene structs
                // (race cameras, viewports) don't exist yet at update 300,
                // which lands in the menus. Each pass overwrites
                // poke_candidates.txt, so the LAST scan before exit wins —
                // park the game in the state of interest.
                if (scan_env && scan_env[0] >= '1' && scan_env[0] <= '9' &&
                    (update_count == 300 || (update_count % 600) == 0)) {
                    poke_scan(app_->core.RDRAM, scan_env[0] - '0');
                }
                if (!s_poke_loaded) {
                    poke_load();
                }

                // Launcher FOV override runs BEFORE poke_apply so that
                // kind-2 (FOV) entries from poke_candidates.txt or WR64_POKE_FOV
                // cannot overwrite the user's slider value. poke_apply checks
                // cur == original; since we've already written desired, it
                // finds cur != original and skips — launcher always wins.
                //
                // Scans every 120 updates (6s at 20fps) for both 45.0 and 75.0
                // degree floats — both were confirmed live by POKE_FOV telemetry
                // (0x800E98xx = race camera 45° structs, 0x800D7D20 = 75° struct).
                // No back-off: race camera structs allocate AFTER the menus, so
                // early scans find only menu-time values; we need continued scanning
                // to pick up race addresses when they appear.
                if (s_fov_degrees > 0.0f) {
                    // last_written: the bits we last wrote so we can re-apply
                    // when FOV changes mid-session without waiting for the game
                    // to reinitialize the address back to orig.
                    struct FovEntry { uint32_t addr, orig, last_written; };
                    static std::vector<FovEntry> s_fov_addrs;
                    static uint32_t s_fov_next_scan = 1;
                    uint8_t* fov_rdram = app_->core.RDRAM;
                    if (update_count >= s_fov_next_scan) {
                        s_fov_next_scan = update_count + 120u;
                        for (uint32_t g = 0x80000010; g < 0x807FFFF0; g += 4) {
                            uint32_t w = rd32g(fov_rdram, g);
                            if (w != 0x42340000u && w != 0x42960000u) continue;
                            bool known = false;
                            for (auto& e : s_fov_addrs)
                                if (e.addr == g) { known = true; break; }
                            if (!known && s_fov_addrs.size() < 512)
                                s_fov_addrs.push_back({g, w, w});
                        }
                    }
                    float desired_f = s_fov_degrees;
                    for (auto& e : s_fov_addrs) {
                        uint32_t cur = rd32g(fov_rdram, e.addr);
                        // Apply if game wrote back the original value OR if the
                        // address still holds what we last wrote (so a slider
                        // change mid-session takes effect immediately).
                        if (cur == e.orig || cur == e.last_written) {
                            float scaled = desired_f * (bits_to_f(e.orig) / 45.0f);
                            uint32_t bits = f_to_bits(scaled);
                            if (bits != cur) {
                                wr32g(fov_rdram, e.addr, bits);
                                e.last_written = bits;
                            }
                        }
                    }
                }

                if (s_poke_hi > s_poke_lo) {
                    poke_apply(app_->core.RDRAM);
                }
                static const char* frustum_env = std::getenv("WR64_POKE_FRUSTUM");
                if (frustum_env && frustum_env[0] == '1') {
                    poke_frustum(app_->core.RDRAM);
                }

                // WR64_PEEK=addr[,addr...] (hex): every 300 updates, log 8
                // words at each address (as u32 and float) — generic RE tool.
                static const char* peek_env = std::getenv("WR64_PEEK");
                if (peek_env && (update_count % 300) == 0) {
                    const char* p = peek_env;
                    while (*p) {
                        uint32_t addr = (uint32_t)strtoul(p, nullptr, 16);
                        if (addr >= 0x80000000u && addr < 0x807FFFE0u) {
                            fprintf(stderr, "[PEEK] 0x%08X:", addr);
                            for (int k = 0; k < 8; k++) {
                                uint32_t w = rd32g(app_->core.RDRAM, addr + k * 4);
                                fprintf(stderr, " %08X(%.3f)", w, bits_to_f(w));
                            }
                            fprintf(stderr, "\n");
                        }
                        const char* c = strchr(p, ',');
                        if (!c) break;
                        p = c + 1;
                    }
                }
                // WR64_POKE_WORDS=addr:val[,addr:val...] (hex): force words
                // every update — generic RE tool.
                static const char* pokew_env = std::getenv("WR64_POKE_WORDS");
                if (pokew_env) {
                    const char* p = pokew_env;
                    while (*p) {
                        uint32_t addr = (uint32_t)strtoul(p, nullptr, 16);
                        const char* colon = strchr(p, ':');
                        if (!colon) break;
                        uint32_t val = (uint32_t)strtoul(colon + 1, nullptr, 16);
                        if (addr >= 0x80000000u && addr < 0x807FFFF0u) {
                            wr32g(app_->core.RDRAM, addr, val);
                        }
                        const char* c = strchr(p, ',');
                        if (!c) break;
                        p = c + 1;
                    }
                }
                // WR64_POKE_FOV=1: self-contained FOV widening experiment.
                // Scans for camera FOV floats (45.0/75.0 — values proven live
                // by RT64 projection telemetry) shortly after boot and widens
                // every hit 1.3x continuously.
                static const char* fov_env = std::getenv("WR64_POKE_FOV");
                if (fov_env && fov_env[0] != '\0' && fov_env[0] != '0') {
                    // The env value doubles as the widening factor, e.g.
                    // WR64_POKE_FOV=2.0 for a dramatic zoom-out. "1" = 1.3.
                    static bool scale_parsed = false;
                    if (!scale_parsed) {
                        scale_parsed = true;
                        float v = (float)atof(fov_env);
                        if (v > 1.01f && v < 4.0f) {
                            s_fov_scale = v;
                        }
                        fprintf(stderr, "[FOV] widening factor: %.2fx\n", s_fov_scale);
                    }
                    uint8_t* rdram = app_->core.RDRAM;
                    // Rescan every ~10s: camera structs are created per scene
                    // (demo/race cameras don't exist at boot), so a single
                    // early scan misses them.
                    static uint32_t next_scan = 240;
                    if (update_count >= next_scan) {
                        next_scan = update_count + 600;
                        int added = 0;
                        for (uint32_t g = 0x80000010; g < 0x807FFFF0; g += 4) {
                            uint32_t w = rd32g(rdram, g);
                            if (w != 0x42340000u && w != 0x42960000u) continue;
                            bool known = false;
                            for (const auto& e : s_poke_list) {
                                if (e.addr == g) { known = true; break; }
                            }
                            if (!known && s_poke_list.size() < 512) {
                                PokeCandidate c{};
                                c.addr = g; c.kind = 2; c.w[0] = w;
                                s_poke_list.push_back(c);
                                added++;
                                if (update_count > 300) {
                                    // Late arrivals are per-scene camera structs — log them.
                                    fprintf(stderr, "[FOV] late candidate at 0x%08X (%s)\n",
                                        g, (w == 0x42340000u) ? "45.0" : "75.0");
                                }
                            }
                        }
                        // WR64_POKE_FOV_ONLY=addr[,addr...]: restrict pokes to
                        // specific addresses (candidate isolation runs).
                        static const char* only_env = std::getenv("WR64_POKE_FOV_ONLY");
                        if (only_env && only_env[0] != '\0') {
                            std::vector<PokeCandidate> filtered;
                            const char* p = only_env;
                            while (*p) {
                                uint32_t a = (uint32_t)strtoul(p, nullptr, 16);
                                for (const auto& e : s_poke_list) {
                                    if (e.addr == a) filtered.push_back(e);
                                }
                                const char* comma = strchr(p, ',');
                                if (!comma) break;
                                p = comma + 1;
                            }
                            s_poke_list = filtered;
                        }
                        s_poke_lo = 0;
                        s_poke_hi = s_poke_list.size();
                        if (added > 0) {
                            fprintf(stderr, "[FOV] +%d new candidates (total %zu after filter), factor %.2fx\n",
                                added, s_poke_list.size(), s_fov_scale);
                        }
                    }
                }
            }

            if (update_count == 600 || update_count == 1800 || update_count == 3000) {
                const char* dump_env = std::getenv("WR64_FB_DUMP");
                if (dump_env && dump_env[0] == '1') {
                    uint8_t* rdram = app_->core.RDRAM;
                    auto rd16 = [&](uint32_t g) -> uint16_t {
                        return *(uint16_t*)(rdram + ((g ^ 2) - 0x80000000u));
                    };
                    auto* vi_regs = ultramodern::renderer::get_vi_regs();
                    uint32_t fb_guest = 0x80000000u | (vi_regs->VI_ORIGIN_REG & 0x3FFFFFu);
                    const char* name = (update_count == 600) ? "fb_dump.bin"
                                     : (update_count == 1800) ? "fb_dump2.bin" : "fb_dump3.bin";
                    FILE* fb = fopen(name, "wb");
                    if (fb) {
                        for (uint32_t i = 0; i < 320u * 240u; i++) {
                            uint16_t px = rd16(fb_guest + i * 2);
                            fwrite(&px, 2, 1, fb);
                        }
                        fclose(fb);
                        fprintf(stderr, "[WR64] framebuffer dumped from 0x%08X\n", fb_guest);
                    }
                }
            }
            app_->updateScreen();
        }
    }

    void shutdown() override {
        if (app_) {
            app_->end();
            app_.reset();
        }
    }

    uint32_t get_display_framerate() const override {
        if (app_ && app_->presentQueue) {
            return app_->presentQueue->ext.sharedResources->swapChainRate;
        }
        // Wave Race 64 targets 30fps (NTSC) — use as fallback.
        return 30;
    }

    float get_resolution_scale() const override {
        // TODO: Return configurable resolution scale from RT64.
        return 1.0f;
    }

private:
    std::unique_ptr<RT64::Application> app_;
};

// ---------------------------------------------------------------------------
// Factory function
// ---------------------------------------------------------------------------

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
    uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode
) {
    // WR64_HUD=stretch|centered: HUD aspect under widescreen expansion.
    // "centered" (default) keeps HUD elements proportional (RT64 AUTO);
    // "stretch" anchors them to the window edges by stretching with the frame.
    // Translated to RT64's rect-aspect default before the first frame.
    const char* hud_env = std::getenv("WR64_HUD");
    bool hud_stretch = (hud_env && strcmp(hud_env, "stretch") == 0);
    if (hud_env && !hud_stretch && strcmp(hud_env, "centered") != 0 && strcmp(hud_env, "auto") != 0 && hud_env[0] != '\0') {
        fprintf(stderr, "[WR64] Unknown WR64_HUD value '%s' (expected 'stretch' or 'centered'); using centered.\n", hud_env);
    }
    if (hud_stretch) {
        _putenv_s("RT64_RECT_ASPECT_DEFAULT", "stretch");
    }
    fprintf(stderr, "[WR64] HUD aspect mode: %s\n", hud_stretch ? "stretch" : "centered");

    // WR64_DEV=1 enables RT64's developer inspector (toggle in-game with F1).
    const char* dev_env = std::getenv("WR64_DEV");
    if (dev_env && dev_env[0] == '1') {
        developer_mode = true;
    }
    s_dev_mode.store(developer_mode);

    auto ctx = std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
    if (!ctx->valid()) {
        fprintf(stderr, "[WR64-RT64] Failed to create render context (result=%d)\n",
                static_cast<int>(ctx->get_setup_result()));
        return nullptr;
    }
    return ctx;
}

bool rt64_handle_sdl_event(void* sdl_event) {
    if (!s_dev_mode.load()) {
        return false;
    }
    RT64::Application* app = s_app.load();
    if (app == nullptr || sdl_event == nullptr) {
        return false;
    }
    return app->sdlEventFilter(static_cast<SDL_Event*>(sdl_event));
}

uint32_t rt64_consume_frame_count() {
    return s_frame_count.exchange(0, std::memory_order_relaxed);
}

bool rt64_texture_pack_loaded() {
    return s_tex_pack_loaded.load(std::memory_order_relaxed);
}

bool rt64_replacements_enabled() {
    RT64::Application* app = s_app.load();
    return app != nullptr && app->textureCache != nullptr &&
           app->textureCache->textureMap.replacementMapEnabled;
}

} // namespace wr64
