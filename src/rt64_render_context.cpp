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
#include <atomic>
#include <vector>

#include <SDL2/SDL_events.h>

#include "ultramodern/renderer_context.hpp"
#include "ultramodern/config.hpp"

#include "hle/rt64_application.h"

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
    static bool logged = false;
    for (int i = 0; i < 8; i++) {
        uint32_t entry = BASE + i * STRIDE;
        if (rd32g(rdram, entry) == 0) continue;
        if (!logged) {
            logged = true;
            fprintf(stderr, "[FRUSTUM] entry %d active: int4=%d r=%f t=%f\n", i,
                (int32_t)rd32g(rdram, entry + 0x04),
                bits_to_f(rd32g(rdram, entry + 0x1C)),
                bits_to_f(rd32g(rdram, entry + 0x14)));
        }

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
            // Aspect constant: widen 1.3x (visible zoom-out / wider view).
            if (rd32g(rdram, c.addr) == c.w[0]) {
                wr32g(rdram, c.addr, f_to_bits(bits_to_f(c.w[0]) * 1.3f));
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

        // Enable developer/debug mode if requested.
        app_->userConfig.developerMode = developer_mode;

        // Widescreen: expand the 3D aspect ratio to fill the window (same
        // mechanism Zelda64Recomp uses). Known limitation without game patches:
        // objects can pop in at the screen edges because the game culls against
        // the original 4:3 frustum. Set WR64_WIDESCREEN=0 to force 4:3.
        const char* ws_env = std::getenv("WR64_WIDESCREEN");
        if (!(ws_env && ws_env[0] == '0')) {
            app_->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
        }

        // Experimental high-FPS: present at the display's refresh rate and let
        // RT64 interpolate transforms between the game's native 20 Hz frames
        // (the Zelda64Recomp approach; no 60fps GameShark code ever existed
        // for this game — its logic rate is hard-coded). WR64_HIGHFPS=1.
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
        // TODO: Map GraphicsConfig fields to RT64's UserConfiguration and apply changes.
        (void)old_config;
        (void)new_config;
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

        // Border removal (EXPERIMENTAL, opt-in via WR64_BORDERS=0): the game
        // scissors rendering to ~(8,20)-(310,218), drawing black CRT-overscan
        // borders inside its own framebuffer. Rewriting the G_SETSCISSOR
        // commands here reveals the full render, BUT the game also culls
        // objects and the detailed wave mesh against the original view rect,
        // so the revealed margins currently show only the flat ocean with a
        // visible color seam (user-verified). Proper removal needs the game's
        // view-bounds variables widened via game patches — until then the
        // original borders remain the default.
        {
            static const char* borders_env = std::getenv("WR64_BORDERS");
            if (borders_env && borders_env[0] == '0') {
                uint8_t* rdram = app_->core.RDRAM;
                uint32_t addr = dl_start_phys;
                for (int i = 0; i < 0x4000; i++, addr += 8) {
                    uint32_t w0 = *(uint32_t*)(rdram + addr);
                    uint8_t op = w0 >> 24;
                    if (op == 0xB8) { // G_ENDDL (F3DEX)
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
                    if (op == 0xED) { // G_SETSCISSOR, coords are 10.2 fixed
                        uint32_t w1 = *(uint32_t*)(rdram + addr + 4);
                        uint32_t x0 = (w0 >> 12) & 0xFFF, y0 = w0 & 0xFFF;
                        uint32_t x1 = (w1 >> 12) & 0xFFF, y1 = w1 & 0xFFF;
                        if (x0 <= 40 && y0 <= 88 && x1 >= 1200 && y1 >= 856) {
                            *(uint32_t*)(rdram + addr) = 0xED000000u;
                            *(uint32_t*)(rdram + addr + 4) = (w1 & 0xFF000000u) | (1280u << 12) | 960u;
                        }
                    }
                }
            }
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

    void update_screen() override {
        if (app_) {
            // Diagnostics: WR64_FB_DUMP=1 dumps the 320x240 framebuffer to
            // fb_dump.bin / fb_dump2.bin / fb_dump3.bin at ~10s/~30s/~50s, for
            // offline analysis with scripts/measure_borders.py.
            static uint32_t update_count = 0;
            ++update_count;

            // Culling-bounds bisection harness (see scripts/bisect_culling.py).
            {
                const char* scan_env = std::getenv("WR64_POKE_SCAN");
                if (scan_env && scan_env[0] >= '1' && scan_env[0] <= '9' && update_count == 300) {
                    poke_scan(app_->core.RDRAM, scan_env[0] - '0');
                }
                if (!s_poke_loaded) {
                    poke_load();
                }
                if (s_poke_hi > s_poke_lo) {
                    poke_apply(app_->core.RDRAM);
                }
                static const char* frustum_env = std::getenv("WR64_POKE_FRUSTUM");
                if (frustum_env && frustum_env[0] == '1') {
                    poke_frustum(app_->core.RDRAM);
                }
                // WR64_POKE_FOV=1: self-contained FOV widening experiment.
                // Scans for camera FOV floats (45.0/75.0 — values proven live
                // by RT64 projection telemetry) shortly after boot and widens
                // every hit 1.3x continuously.
                static const char* fov_env = std::getenv("WR64_POKE_FOV");
                if (fov_env && fov_env[0] == '1') {
                    uint8_t* rdram = app_->core.RDRAM;
                    static bool fov_scanned = false;
                    if (!fov_scanned && update_count >= 240) {
                        fov_scanned = true;
                        int found = 0;
                        for (uint32_t g = 0x80000010; g < 0x807FFFF0 && found < 256; g += 4) {
                            uint32_t w = rd32g(rdram, g);
                            if (w == 0x42340000u || w == 0x42960000u) {
                                PokeCandidate c{};
                                c.addr = g; c.kind = 2; c.w[0] = w;
                                s_poke_list.push_back(c);
                                found++;
                            }
                        }
                        s_poke_lo = 0;
                        s_poke_hi = s_poke_list.size();
                        fprintf(stderr, "[FOV] widening %d camera FOV candidates 1.3x\n", found);
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

} // namespace wr64
