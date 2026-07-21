/**
 * @file main.cpp
 * @brief WaveRace64-Recomp game integration entry point.
 *
 * Registers the Wave Race 64 GameEntry with the N64ModernRuntime,
 * builds the overlay section table, wires up all callbacks, and
 * calls recomp::start() to launch the game.
 */

#include <cstdio>
#include <cstdint>
#include <string>
#include <thread>
#include <chrono>
#include <vector>
#include <fstream>
#include <filesystem>
#include <system_error>

#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "librecomp/sections.h"
#include "librecomp/addresses.hpp"

// Game-specific headers
#include "rt64_render_context.h"
#include "rsp_microcode.h"
#include "audio.h"
#include "input.h"

#ifdef HAS_RECOMPUI
#include "recompui/recompui.h"
#include "recompui/program_config.h"
#include "recompui/config.h"
#include "recompinput/input_events.h"
#include "recompinput/players.h"
#include "util/file.h"   // recompui folder/file pickers (NFD-backed)
#include "core/ui_context.h"     // create_context (FPS overlay)
#include "elements/ui_label.h"
#include "nfd.h"

// WR64-specific renderer settings exposed by rt64_render_context.cpp.
extern void wr64_set_show_borders(bool show);
extern void wr64_set_wavegrid(uint32_t rows, uint32_t cols);
extern void wr64_set_fov_degrees(float deg);
extern "C" void wr64_set_wave_interp(bool enabled);
extern "C" void wr64_set_overscan_crop(bool enabled);

// FPS overlay configuration (set from the launcher Enhancements tab on the UI
// thread, consumed by the overlay in update_gfx on the gfx thread).
static std::atomic<bool>     s_fps_show{true};
static std::atomic<uint32_t> s_fps_pos{0};      // 0 TR, 1 TL, 2 BR, 3 BL
static std::atomic<uint32_t> s_fps_color{0};    // index into wr64_fps_colors
static std::atomic<uint32_t> s_fps_opacity{45}; // background opacity percent
static std::atomic<uint32_t> s_fps_cfg_gen{1};
extern void wr64_set_texture_pack(const char* dir);
extern void wr64_set_texture_replace(bool enabled);
extern void wr64_set_texture_dump(bool enabled);
extern void wr64_mod_texture_pack_enabled(const std::string& mod_id);
extern void wr64_mod_texture_pack_disabled(const std::string& mod_id);
extern void wr64_mod_texture_packs_reordered();
#endif

// Pull in the recompiled function declarations and overlay tables.
#include "recomp_overlays.inl"

// Forward-declare the recomp entrypoint (defined in RecompiledFuncs/funcs.h,
// already included transitively through recomp_overlays.inl -> funcs.h).

// ---------------------------------------------------------------------------
// Wave Race 64 (USA Rev 1) constants
// ---------------------------------------------------------------------------
static constexpr uint64_t WR64_ROM_HASH          = 0x2B675E2250A604FCull;
static constexpr gpr      WR64_ENTRYPOINT_ADDR   = (gpr)(int32_t)0x80046800u;
static constexpr const char* WR64_INTERNAL_NAME  = "WAVE RACE 64";
// Game uses 4-Kbit EEPROM for saves.

// ---------------------------------------------------------------------------
// Section table
// ---------------------------------------------------------------------------
// Section data extracted from waverace64.us.rev1.syms.toml.
// The FuncEntry arrays are defined in recomp_overlays.inl.
//
// 19 code sections (indices 0-18):
//   0  .main            rom=0x00001000  vram=0x80046800  size=0xA85D0
//   1  .codeseg         rom=0x000A95D0  vram=0x801DAFA0  size=0x4CAC0
//   2  .segment_1B1FB0  rom=0x001B1FB0  vram=0x802C5800  size=0x1F10
//   3  .ovl_i0          rom=0x001B3EC0  vram=0x802C5800  size=0x16E0
//   4  .ovl_i1          rom=0x001B55A0  vram=0x802C5800  size=0x3EA0
//   5  .ovl_i2          rom=0x001B9440  vram=0x802C5800  size=0x3450
//   6  .ovl_i3          rom=0x001BC890  vram=0x802C5800  size=0x1820
//   7  .ovl_i4          rom=0x001BE0B0  vram=0x802C5800  size=0x1EA0
//   8  .ovl_i5          rom=0x001BFF50  vram=0x802C5800  size=0x2300
//   9  .ovl_i6          rom=0x001C2250  vram=0x802C5800  size=0x1530
//  10  .seg_1C3780      rom=0x001C3780  vram=0x802C5800  size=0x580
//  11  .seg_1C3D00      rom=0x001C3D00  vram=0x802C5800  size=0x6F0
//  12  .ovl_i7          rom=0x001C43F0  vram=0x802C5800  size=0x5B0
//  13  .ovl_i8          rom=0x001C49A0  vram=0x802C5800  size=0x1D30
//  14  .ovl_i9          rom=0x001C66D0  vram=0x802C5800  size=0x2A80
//  15  .ovl_i10         rom=0x001C9150  vram=0x802C5800  size=0x1330
//  16  .ovl_i11         rom=0x001CA480  vram=0x802C5800  size=0x9C0
//  17  .ovl_i12         rom=0x001CAE40  vram=0x802C5800  size=0xCB0
//  18  .ovl_i13         rom=0x001CBAF0  vram=0x802C5800  size=0x3690

static SectionTableEntry code_sections[] = {
    { .rom_addr = 0x00001000, .ram_addr = 0x80046800, .size = 0xA85D0,
      .funcs = section_0_main_funcs, .num_funcs = ARRLEN(section_0_main_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 0 },
    { .rom_addr = 0x000A95D0, .ram_addr = 0x801DAFA0, .size = 0x4CAC0,
      .funcs = section_1_codeseg_funcs, .num_funcs = ARRLEN(section_1_codeseg_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 1 },
    { .rom_addr = 0x001B1FB0, .ram_addr = 0x802C5800, .size = 0x1F10,
      .funcs = section_2_segment_1B1FB0_funcs, .num_funcs = ARRLEN(section_2_segment_1B1FB0_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 2 },
    { .rom_addr = 0x001B3EC0, .ram_addr = 0x802C5800, .size = 0x16E0,
      .funcs = section_3_ovl_i0_funcs, .num_funcs = ARRLEN(section_3_ovl_i0_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 3 },
    { .rom_addr = 0x001B55A0, .ram_addr = 0x802C5800, .size = 0x3EA0,
      .funcs = section_4_ovl_i1_funcs, .num_funcs = ARRLEN(section_4_ovl_i1_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 4 },
    { .rom_addr = 0x001B9440, .ram_addr = 0x802C5800, .size = 0x3450,
      .funcs = section_5_ovl_i2_funcs, .num_funcs = ARRLEN(section_5_ovl_i2_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 5 },
    { .rom_addr = 0x001BC890, .ram_addr = 0x802C5800, .size = 0x1820,
      .funcs = section_6_ovl_i3_funcs, .num_funcs = ARRLEN(section_6_ovl_i3_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 6 },
    { .rom_addr = 0x001BE0B0, .ram_addr = 0x802C5800, .size = 0x1EA0,
      .funcs = section_7_ovl_i4_funcs, .num_funcs = ARRLEN(section_7_ovl_i4_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 7 },
    { .rom_addr = 0x001BFF50, .ram_addr = 0x802C5800, .size = 0x2300,
      .funcs = section_8_ovl_i5_funcs, .num_funcs = ARRLEN(section_8_ovl_i5_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 8 },
    { .rom_addr = 0x001C2250, .ram_addr = 0x802C5800, .size = 0x1530,
      .funcs = section_9_ovl_i6_funcs, .num_funcs = ARRLEN(section_9_ovl_i6_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 9 },
    { .rom_addr = 0x001C3780, .ram_addr = 0x802C5800, .size = 0x580,
      .funcs = section_10_seg_1C3780_funcs, .num_funcs = ARRLEN(section_10_seg_1C3780_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 10 },
    { .rom_addr = 0x001C3D00, .ram_addr = 0x802C5800, .size = 0x6F0,
      .funcs = section_11_seg_1C3D00_funcs, .num_funcs = ARRLEN(section_11_seg_1C3D00_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 11 },
    { .rom_addr = 0x001C43F0, .ram_addr = 0x802C5800, .size = 0x5B0,
      .funcs = section_12_ovl_i7_funcs, .num_funcs = ARRLEN(section_12_ovl_i7_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 12 },
    { .rom_addr = 0x001C49A0, .ram_addr = 0x802C5800, .size = 0x1D30,
      .funcs = section_13_ovl_i8_funcs, .num_funcs = ARRLEN(section_13_ovl_i8_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 13 },
    { .rom_addr = 0x001C66D0, .ram_addr = 0x802C5800, .size = 0x2A80,
      .funcs = section_14_ovl_i9_funcs, .num_funcs = ARRLEN(section_14_ovl_i9_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 14 },
    { .rom_addr = 0x001C9150, .ram_addr = 0x802C5800, .size = 0x1330,
      .funcs = section_15_ovl_i10_funcs, .num_funcs = ARRLEN(section_15_ovl_i10_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 15 },
    { .rom_addr = 0x001CA480, .ram_addr = 0x802C5800, .size = 0x9C0,
      .funcs = section_16_ovl_i11_funcs, .num_funcs = ARRLEN(section_16_ovl_i11_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 16 },
    { .rom_addr = 0x001CAE40, .ram_addr = 0x802C5800, .size = 0xCB0,
      .funcs = section_17_ovl_i12_funcs, .num_funcs = ARRLEN(section_17_ovl_i12_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 17 },
    { .rom_addr = 0x001CBAF0, .ram_addr = 0x802C5800, .size = 0x3690,
      .funcs = section_18_ovl_i13_funcs, .num_funcs = ARRLEN(section_18_ovl_i13_funcs),
      .relocs = nullptr, .num_relocs = 0, .index = 18 },
};

static constexpr size_t NUM_CODE_SECTIONS  = ARRLEN(code_sections);
// Total sections including potential data/BSS sections referenced by relocs.
// For now, equals code section count since we have no relocation data.
static constexpr size_t TOTAL_NUM_SECTIONS = 21; // 19 code + potential data/BSS sections

// ---------------------------------------------------------------------------
// SDL2 window / gfx callbacks
// ---------------------------------------------------------------------------
#include <SDL2/SDL.h>
#ifdef _WIN32
#include <SDL2/SDL_syswm.h>
#endif

#include "register_patches.h"

// Must be non-static so recompui can reference it for window-size queries.
SDL_Window* window = nullptr;

// Required by RecompFrontend's ui_launcher.cpp (used only when no custom
// launcher_init_callback is registered; we register our own so this stays empty).
std::vector<recomp::GameEntry> supported_games;

static std::atomic<bool> s_quit_requested{false};

static int sdl_quit_watch(void* /*userdata*/, SDL_Event* event) {
    if (event->type == SDL_QUIT) {
        s_quit_requested.store(true);
    }
    return 0;
}

static void* create_gfx() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[WR64] SDL_Init failed: %s\n", SDL_GetError());
        return nullptr;
    }
    SDL_AddEventWatch(sdl_quit_watch, nullptr);
    return nullptr; // gfx_data not used
}

static ultramodern::renderer::WindowHandle create_window(void* /*gfx_data*/) {
#ifdef _WIN32
    // On Windows RT64 attaches via the native HWND (D3D12/Vulkan chosen at
    // runtime), so SDL_WINDOW_VULKAN must not be forced here.
    constexpr Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    constexpr Uint32 window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
#endif
    // Size the window to fit the display's usable area (fixed 1280x960 is
    // taller than many laptop screens once DPI scaling and the taskbar are
    // accounted for, leaving the title bar off-screen).
    // WR64_WINDOW=WxH overrides (e.g. 1600x900 for widescreen testing).
    int win_w = 1280, win_h = 960;
    if (const char* win_env = SDL_getenv("WR64_WINDOW")) {
        int w = 0, h = 0;
        if (SDL_sscanf(win_env, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
            win_w = w;
            win_h = h;
        }
    }
    SDL_Rect usable{};
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.w > 0 && usable.h > 0) {
        int max_h = (int)(usable.h * 0.90f);
        int max_w = (int)(usable.w * 0.95f);
        if (win_h > max_h) { win_h = max_h; win_w = win_h * 4 / 3; }
        if (win_w > max_w) { win_w = max_w; win_h = win_w * 3 / 4; }
    }
    window = SDL_CreateWindow(
        "Wave Race 64",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        win_w, win_h,
        window_flags
    );
    if (!window) {
        fprintf(stderr, "[WR64] SDL_CreateWindow failed: %s\n", SDL_GetError());
    }
#ifdef _WIN32
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.win.window, GetCurrentThreadId() };
#else
    return window;
#endif
}

static void update_gfx(void* /*gfx_data*/) {
#ifdef HAS_RECOMPUI
    // handle_events() pumps the SDL queue and routes each event through
    // recompinput's sdl_event_filter, which handles: F11 / Alt+Enter
    // fullscreen toggle, controller device add/remove, binding-mode
    // detection (joystick remapping), cursor visibility, and queuing to
    // recompui for RmlUi. SDL_QUIT is caught by the sdl_quit_watch
    // registered in create_gfx and applied below.
    recompinput::handle_events();

    if (s_quit_requested.exchange(false)) {
        ultramodern::quit();
    }
#else
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                ultramodern::quit();
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    ultramodern::quit();
                }
                break;
            default:
                break;
        }
    }
#endif

    // One-shot safeguard: if the window ends up with its title bar off-screen
    // (can happen with DPI/multi-monitor centering and post-setup resizes),
    // pull it back into view. Checked ~2s in so RT64's setup has settled.
    static uint32_t position_check_ticks = 0;
    if (position_check_ticks != UINT32_MAX && window != nullptr) {
        uint32_t now_ticks = SDL_GetTicks();
        if (position_check_ticks == 0) {
            position_check_ticks = now_ticks;
        } else if (now_ticks - position_check_ticks > 2000) {
            position_check_ticks = UINT32_MAX;
            int x = 0, y = 0;
            SDL_GetWindowPosition(window, &x, &y);
            if (y < 0 || x < -100) {
                SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            }
        }
    }

    // F5: toggle original <-> replaced textures (global — all loaded packs,
    // Mods tab and Textures tab alike). Edge-detected from the SDL keyboard
    // state so it works during gameplay AND with the menu open. Runtime-only;
    // it never touches the persisted tex_enable / mod settings. (F4 does the
    // same inside RT64 but only in developer mode.)
    {
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        static bool f5_was_down = false;
        const bool f5_down = keys[SDL_SCANCODE_F5] != 0;
        if (f5_down && !f5_was_down && wr64::rt64_texture_pack_loaded()) {
            bool now_on = wr64::rt64_toggle_replacements();
#ifdef HAS_RECOMPUI
            recompui::config::show_notification(recompui::config::NotificationType::Info,
                now_on ? "HD textures: ON" : "HD textures: OFF (original)");
#endif
        }
        f5_was_down = f5_down;
    }

    // Title bar: friendly static info (target framerate + texture-pack state);
    // the measured FPS lives in the in-window overlay below. Refreshed on a 1s
    // tick and the instant the TEX state changes (e.g. F4) so it never lags.
    static uint32_t last_ticks = 0;
    static float    last_fps = 0.0f;          // game-frame rate (~20), gates the overlay
    static float    last_present_fps = 0.0f;  // presented rate (incl. interpolation)
    static int      last_tex = -2;  // -1 no pack, 0 off, 1 on; -2 = force first draw
    uint32_t now = SDL_GetTicks();
    bool tick = (now - last_ticks >= 1000);
    if (tick) {
        uint32_t frames = wr64::rt64_consume_frame_count();
        uint32_t presents = wr64::rt64_consume_present_count();
        if (last_ticks != 0) {
            last_fps = frames * 1000.0f / (now - last_ticks);
            last_present_fps = presents * 1000.0f / (now - last_ticks);
        }
        last_ticks = now;
    }
    int tex = wr64::rt64_texture_pack_loaded()
        ? (wr64::rt64_replacements_enabled() ? 1 : 0) : -1;
    if (window != nullptr && (tick || tex != last_tex) && last_fps > 0.0f) {
        last_tex = tex;
        uint32_t target = ultramodern::get_target_framerate(60);
        const char* texs = (tex < 0) ? ""
                         : (tex == 1) ? "   \xE2\x80\xA2   HD Textures: On"
                                      : "   \xE2\x80\xA2   HD Textures: Off";
        char title[160];
        SDL_snprintf(title, sizeof(title),
                     "Wave Race 64 - Recompiled   \xE2\x80\xA2   Target: %u FPS   "
                     "\xE2\x80\xA2   Native: %.0f FPS%s",
                     target, last_fps, texs);
        SDL_SetWindowTitle(window, title);
    }

#ifdef HAS_RECOMPUI
    // In-window FPS readout: a small always-on overlay pill, updated on the
    // same 1s tick. Shows the presented (interpolated) rate plus the game's
    // native rate in parentheses. Configurable in Enhancements (on/off,
    // corner, color); the font is sized in raw pixels with a floor so it stays
    // readable in small windows (the UI's dp scale would shrink it). Lives in
    // its own recompui context that never captures input (a capturing shown
    // context would disable all in-game input via recompinput).
    if (tick && last_fps > 0.0f) {
        static recompui::ContextId fps_ctx = {};
        static bool fps_ctx_created = false;
        static recompui::Element* fps_pill = nullptr;
        static recompui::Label* fps_label = nullptr;
        static bool fps_shown = false;
        static uint32_t fps_applied_gen = 0;

        if (!fps_ctx_created) {
            fps_ctx = recompui::create_context();
            fps_ctx.set_captures_input(false);
            fps_ctx.set_captures_mouse(false);
            fps_ctx_created = true;
        }

        const bool show = s_fps_show.load();
        const uint32_t gen = s_fps_cfg_gen.load();
        if (!show) {
            if (fps_shown) {
                recompui::hide_context(fps_ctx);
                fps_shown = false;
            }
        } else {
            fps_ctx.open();
            if (gen != fps_applied_gen) {
                fps_applied_gen = gen;
                // Rebuild the pill with the current position/color config.
                if (fps_pill != nullptr) {
                    fps_ctx.get_root_element()->remove_child(fps_pill);
                    fps_pill = nullptr;
                    fps_label = nullptr;
                }
                fps_pill = fps_ctx.create_element<recompui::Element>(
                    fps_ctx.get_root_element());
                fps_pill->set_position(recompui::Position::Absolute);
                const uint32_t pos = s_fps_pos.load();
                if (pos == 0 || pos == 1) fps_pill->set_top(8.0f);
                else                      fps_pill->set_bottom(8.0f);
                if (pos == 0 || pos == 2) fps_pill->set_right(14.0f);
                else                      fps_pill->set_left(14.0f);
                fps_pill->set_padding_top(2.0f);
                fps_pill->set_padding_bottom(2.0f);
                fps_pill->set_padding_left(10.0f);
                fps_pill->set_padding_right(10.0f);
                fps_pill->set_border_radius(10.0f);
                uint32_t op = s_fps_opacity.load();
                if (op > 100) op = 100;
                fps_pill->set_background_color(recompui::Color{ 0, 0, 0, uint8_t(op * 255 / 100) });
                fps_label = fps_ctx.create_element<recompui::Label>(
                    fps_pill, "", recompui::LabelStyle::Small);
                static constexpr recompui::Color fps_colors[] = {
                    { 255, 255, 255, 255 },  // white
                    { 255, 220,  80, 255 },  // yellow
                    { 130, 255, 130, 255 },  // green
                    { 120, 230, 255, 255 },  // cyan
                    { 255, 170,  60, 255 },  // orange
                    { 255, 110, 110, 255 },  // red
                };
                uint32_t ci = s_fps_color.load();
                if (ci >= SDL_arraysize(fps_colors)) ci = 0;
                fps_label->set_color(fps_colors[ci]);
            }

            // Pixel-sized font with a floor so small windows stay readable.
            int win_w = 0, win_h = 0;
            if (window != nullptr) SDL_GetWindowSize(window, &win_w, &win_h);
            float font_px = win_h * 0.022f;
            if (font_px < 15.0f) font_px = 15.0f;
            if (font_px > 30.0f) font_px = 30.0f;
            fps_label->set_font_size(font_px, recompui::Unit::Px);

            char fps_text[48];
            SDL_snprintf(fps_text, sizeof(fps_text), "%.0f FPS (native %.0f)",
                         last_present_fps, last_fps);
            fps_label->set_text(fps_text);
            fps_ctx.close();
            if (!fps_shown) {
                recompui::show_context(fps_ctx, "");
                fps_shown = true;
            }
        }
    }
#endif
}

// ---------------------------------------------------------------------------
// Events callbacks
// ---------------------------------------------------------------------------
static void vi_callback() {
    // Called each VI interrupt. Can be used for frame pacing.
}

static void gfx_init_callback() {
#ifndef HAS_RECOMPUI
    // Without the launcher UI, auto-start the game after the VI thread has
    // populated both ViState slots (set_dummy_vi runs at least once first).
    std::thread([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        std::u8string game_id = u8"waverace64";
        if (recomp::is_rom_valid(game_id)) {
            printf("[WR64] ROM validated, starting game...\n");
            recomp::start_game(game_id);
        } else {
            fprintf(stderr, "[WR64] ERROR: ROM not valid at startup (check waverace64.z64 in CWD)\n");
        }
    }).detach();
#endif
    // With HAS_RECOMPUI: draw_hook shows the launcher automatically on the
    // first frame when no context is shown and the game hasn't started.
}

// ---------------------------------------------------------------------------
// Error handling callback
// ---------------------------------------------------------------------------
static void error_message_box(const char* msg) {
    fprintf(stderr, "[WR64] ERROR: %s\n", msg);
    if (window) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Wave Race 64 - Error", msg, window);
    }
}

// ---------------------------------------------------------------------------
// Unlock-all-courses: edit the EEPROM save file directly. Wave Race 64 gates
// Time-Trial course availability on difficulty completion (not per-course lock
// flags), so we mark all four difficulties finished. Save layout (verified;
// KilianSteenman/N64-Save-file-formats wave-race-64.bt):
//   0x00 s16 magic "TE"        0x02 u16 checksum (BE) = sum(bytes[4..511])&0xFFFF
//   0x08 normal(1->6) 0x09 hard(0->6) 0x0A expert(0->7) 0x0B reverse(0->7)
//   0x0C completion bitfield (didFinish bits = low nibble -> 0x0F; verified:
//        0xF0 muted the menu music, so the four didFinish bits are the low four)
// Takes effect when the game (re)starts (the running game holds its own RAM
// copy read at boot). Returns true on success.
static bool wr64_unlock_all_courses() {
    std::error_code ec;
    std::filesystem::path path = recomp::get_config_path() / "saves" / "waverace64.bin";
    if (!std::filesystem::exists(path, ec)) {
        fprintf(stderr, "[WR64] unlock: no save file at %s (run the game once first)\n",
                path.string().c_str());
        return false;
    }

    std::vector<uint8_t> d;
    {
        std::ifstream in(path, std::ios::binary);
        d.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (d.size() != 0x200 || d[0] != 'T' || d[1] != 'E') {
        fprintf(stderr, "[WR64] unlock: unexpected save format (size=%zu)\n", d.size());
        return false;
    }

    // Keep a backup of the pre-unlock save so records can be restored.
    std::filesystem::path bak = path;
    bak += ".unlock_backup";
    std::filesystem::copy_file(path, bak,
        std::filesystem::copy_options::overwrite_existing, ec);

    d[0x08] = 0x06; d[0x09] = 0x06; d[0x0A] = 0x07; d[0x0B] = 0x07;
    d[0x0C] = 0x0F; // didFinish normal|hard|expert|reverse (low nibble)

    uint32_t ck = 0;
    for (size_t i = 4; i < 0x200; i++) ck += d[i];
    ck &= 0xFFFF;
    d[0x02] = static_cast<uint8_t>(ck >> 8);
    d[0x03] = static_cast<uint8_t>(ck & 0xFF);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        fprintf(stderr, "[WR64] unlock: cannot open save for writing\n");
        return false;
    }
    out.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
    fprintf(stderr, "[WR64] unlock: all courses unlocked in %s (restart to apply)\n",
            path.string().c_str());
    return out.good();
}

// ---------------------------------------------------------------------------
// Threads callback
// ---------------------------------------------------------------------------
static std::string get_game_thread_name(const OSThread* t) {
    int id = t ? static_cast<int>(t->id) : -1;
    switch (id) {
        case 1: return "WR64 Idle";
        case 3: return "WR64 Main";
        case 4: return "WR64 Audio";
        case 5: return "WR64 Sched";
        default: return "WR64 Thread " + std::to_string(id);
    }
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    printf("[WR64] Wave Race 64 PC Recompilation v0.1.0\n");

    // -----------------------------------------------------------------------
    // 1. Register overlay sections
    // -----------------------------------------------------------------------
    recomp::overlays::overlay_section_table_data_t section_table {
        .code_sections    = code_sections,
        .num_code_sections = NUM_CODE_SECTIONS,
        .total_num_sections = TOTAL_NUM_SECTIONS,
    };

    // Overlay-by-index mapping (not needed if overlay loading uses ROM address matching).
    recomp::overlays::overlays_by_index_t overlays_by_index {
        .table = nullptr,
        .len   = 0,
    };

    recomp::overlays::register_overlays(section_table, overlays_by_index);

    // Register the recompiled game patches (RecompiledPatches/).
    wr64::register_patches();

    // -----------------------------------------------------------------------
    // 2. Register the game entry
    // -----------------------------------------------------------------------
    recomp::GameEntry wr64_entry {
        .rom_hash            = WR64_ROM_HASH,
        .internal_name       = WR64_INTERNAL_NAME,
        .display_name        = "Wave Race 64",
        .game_id             = u8"waverace64",
        .mod_game_id         = "waverace64",
        .save_type           = recomp::SaveType::Eep4k,
        .is_enabled          = true,
        .entrypoint_address  = WR64_ENTRYPOINT_ADDR,
        .entrypoint          = recomp_entrypoint,
    };

    if (!recomp::register_game(wr64_entry)) {
        fprintf(stderr, "[WR64] Failed to register game entry!\n");
        return 1;
    }
    printf("[WR64] Game registered: %s (hash 0x%016llX)\n",
           wr64_entry.internal_name.c_str(),
           (unsigned long long)wr64_entry.rom_hash);

    // -----------------------------------------------------------------------
    // 3. Build the Configuration and start
    // -----------------------------------------------------------------------
    recomp::Configuration config {
        .project_version = recomp::Version(0, 1, 0, "-alpha"),

        .window_handle = ultramodern::renderer::WindowHandle{},

        .rsp_callbacks = {
            .get_rsp_microcode = wr64::get_rsp_microcode,
        },

        .renderer_callbacks = {
            .create_render_context = wr64::create_render_context,
        },

        .audio_callbacks = {
            .queue_samples       = wr64::audio_queue_samples,
            .get_frames_remaining = wr64::audio_get_frames_remaining,
            .set_frequency       = wr64::audio_set_frequency,
        },

        .input_callbacks = {
            .poll_input              = wr64::input_poll,
            .get_input               = wr64::input_get,
            .set_rumble              = wr64::input_set_rumble,
            .get_connected_device_info = wr64::input_get_connected_device_info,
        },

        .gfx_callbacks = {
            .create_gfx    = create_gfx,
            .create_window = create_window,
            .update_gfx    = update_gfx,
        },

        .events_callbacks = {
            .vi_callback       = vi_callback,
            .gfx_init_callback = gfx_init_callback,
        },

        .error_handling_callbacks = {
            .message_box = error_message_box,
        },

        .threads_callbacks = {
            .get_game_thread_name = get_game_thread_name,
        },

        .message_queue_control = {
            // Wave Race 64 defaults: requeue timer, sp, si, dp; don't requeue ai, vi, pi.
        },
    };

    printf("[WR64] Starting recomp runtime...\n");
    // Anchor config path to CWD so saves, mods, and ROM cache resolve correctly.
    recomp::register_config_path(std::filesystem::current_path());

#ifdef HAS_RECOMPUI
    NFD_Init();

    recompui::programconfig::set_program_name("Wave Race 64");
    recompui::programconfig::set_program_id(u8"waverace64");
    recompui::register_primary_font("LatoLatin-Regular.ttf", "LatoLatin");

    // Create standard config tabs before finalize (required by RecompFrontend).
    recompui::config::create_general_tab({});
    recompui::config::create_graphics_tab();
    recompui::config::create_controls_tab();
    recompui::config::create_sound_tab();
    // WR64-specific game settings tab.
    {
        recomp::config::Config& wr64_cfg = recompui::config::create_config_tab("Enhancements", "wr64_settings", true);
        // Hidden since the Overscan Crop landed: the stock-borders boot mode is
        // superseded by overscan (which crops the borders properly, 1P and 2P)
        // and its restart-required UX was confusing. Still functional for power
        // users via wr64_settings.json ("show_borders") or WR64_BORDERS=1.
        wr64_cfg.add_bool_option("show_borders", "Show Borders (restart required)",
            "Restore the original 4:3 presentation with the game's black overscan "
            "borders, pillarboxed at correct proportions. Off by default: the image "
            "is widened edge-to-edge to fill the window. Changing this takes effect "
            "after restarting the game (the renderer aspect ratio is fixed at launch).",
            false, /* hidden */ true);
        wr64_cfg.add_number_option("fov_degrees", "Field of View",
            "Camera FOV in degrees. Default 47.75 (wider than original 45°). "
            "Higher values show more of the scene horizontally.",
            40.0, 75.0, 0.25, 2, false, 47.75);
        wr64_cfg.add_button_option("reset_fov", "Reset FOV to Original",
            "Set the Field of View slider back to the original N64 game value "
            "of 45°, then press Apply.",
            "Reset to 45°");
        wr64_cfg.add_enum_option("wave_grid", "Wave Detail Area",
            "How far the detailed foam-water mesh extends into widescreen. "
            "Larger grids fill more of an ultrawide screen at negligible cost.",
            {
                {0u, "stock",    "Stock (19x35)"},
                {1u, "wide",     "Wide (23x55)"},
                {2u, "wider",    "Wider (27x63)"},
                {3u, "widest",   "Widest (32x78)"},
                {4u, "maximum",  "Maximum (40x96)"},
            }, 1u);
        wr64_cfg.add_bool_option("overscan_crop", "Overscan Crop",
            "Crop the TV-overscan margins during gameplay (with scale-up), like "
            "a real television did: the border bands and the garbage strip at "
            "the frame edges disappear and the game fills the whole window. "
            "The 3D view keeps its proportions; the HUD gets the original TV "
            "framing (slightly larger).",
            true);
        wr64_cfg.add_bool_option("fps_display", "FPS Display",
            "Show a frame-rate readout in the corner of the game window: the "
            "presented (interpolated) rate plus the game's native rate in "
            "parentheses.",
            true);
        wr64_cfg.add_enum_option("fps_position", "FPS Position",
            "Corner of the window for the FPS readout.",
            {
                {0u, "top_right",    "Top Right"},
                {1u, "top_left",     "Top Left"},
                {2u, "bottom_right", "Bottom Right"},
                {3u, "bottom_left",  "Bottom Left"},
            }, 0u);
        wr64_cfg.add_enum_option("fps_color", "FPS Color",
            "Text color of the FPS readout.",
            {
                {0u, "white",  "White"},
                {1u, "yellow", "Yellow"},
                {2u, "green",  "Green"},
                {3u, "cyan",   "Cyan"},
                {4u, "orange", "Orange"},
                {5u, "red",    "Red"},
            }, 0u);
        wr64_cfg.add_percent_number_option("fps_opacity", "FPS Background Opacity",
            "Opacity of the dark pill behind the FPS readout. 0% removes the "
            "background entirely.",
            45.0);
        // The FPS styling options only matter while the readout is enabled.
        wr64_cfg.add_option_hidden_dependency("fps_position", "fps_display", false);
        wr64_cfg.add_option_hidden_dependency("fps_color", "fps_display", false);
        wr64_cfg.add_option_hidden_dependency("fps_opacity", "fps_display", false);
        wr64_cfg.add_bool_option("wave_interp", "Smooth Water (experimental)",
            "Interpolate the wave mesh at high framerates so the water slides "
            "smoothly instead of stepping at the game's native 20 Hz. "
            "Experimental: the wave motion can look off — turn it off if the "
            "water seems wrong. Clouds and sprites are always smoothed.",
            false);
        wr64_cfg.add_button_option("unlock_courses", "Unlock All Courses",
            "Mark every difficulty complete in the save file, unlocking all "
            "courses in Time Trials. Takes effect when the game (re)starts; "
            "best pressed before starting the game. A backup of the previous "
            "save is kept.",
            "Unlock");

        // Buttons act immediately when pressed (Permanent context; never fired
        // at config load).
        wr64_cfg.add_option_change_callback("reset_fov",
            [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant,
               recomp::config::OptionChangeContext ctx) {
                if (ctx == recomp::config::OptionChangeContext::Load) return;
                // Snap the slider back to 45; lands in the pending (temp) config
                // so Apply persists it like any manual slider change.
                recompui::config::get_config("wr64_settings").update_option_value("fov_degrees", 45.0);
                recompui::config::show_notification(recompui::config::NotificationType::Info,
                    "Field of View set to 45° — press Apply to save.");
            });
        wr64_cfg.add_option_change_callback("unlock_courses",
            [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant,
               recomp::config::OptionChangeContext ctx) {
                if (ctx == recomp::config::OptionChangeContext::Load) return;
                if (wr64_unlock_all_courses()) {
                    recompui::config::show_notification(recompui::config::NotificationType::Success,
                        "All courses unlocked — takes effect when the game (re)starts.");
                } else {
                    recompui::config::show_notification(recompui::config::NotificationType::Error,
                        "Unlock failed — no save file yet? Start the game once first.");
                }
            });

        auto apply_wr64 = []() {
            recomp::config::Config& cfg = recompui::config::get_config("wr64_settings");
            wr64_set_show_borders(std::get<bool>(cfg.get_option_value("show_borders")));

            float fov = static_cast<float>(std::get<double>(cfg.get_option_value("fov_degrees")));
            wr64_set_fov_degrees(fov);

            static constexpr uint32_t rows[] = {19, 23, 27, 32, 40};
            static constexpr uint32_t cols[] = {35, 55, 63, 78, 96};
            uint32_t idx = std::get<uint32_t>(cfg.get_option_value("wave_grid"));
            if (idx >= 5) idx = 1;
            wr64_set_wavegrid(rows[idx], cols[idx]);

            wr64_set_wave_interp(std::get<bool>(cfg.get_option_value("wave_interp")));
            wr64_set_overscan_crop(std::get<bool>(cfg.get_option_value("overscan_crop")));

            s_fps_show.store(std::get<bool>(cfg.get_option_value("fps_display")));
            s_fps_pos.store(std::get<uint32_t>(cfg.get_option_value("fps_position")));
            s_fps_color.store(std::get<uint32_t>(cfg.get_option_value("fps_color")));
            s_fps_opacity.store(uint32_t(std::get<double>(cfg.get_option_value("fps_opacity"))));
            s_fps_cfg_gen.fetch_add(1);
        };
        wr64_cfg.set_load_callback(apply_wr64);
        wr64_cfg.set_save_callback(apply_wr64);
    }
    // Textures tab: HD texture-replacement packs.
    {
        recomp::config::Config& tex_cfg = recompui::config::create_config_tab("Textures", "wr64_textures", true);
        tex_cfg.add_bool_option("tex_enable", "Enable Texture Pack",
            "Load HD replacement textures from the folder below. Only affects "
            "this pack — texture-pack mods in the Mods tab have their own "
            "toggles. Applies live; F5 toggles all replacements in-game.",
            true);
        tex_cfg.add_string_option("tex_pack_dir", "Texture Pack Folder or .zip",
            "Path to a texture pack — either a folder or a .zip file (both need an "
            "rt64.json inside, or the Rice database). Relative paths are resolved "
            "from the game's folder. Overridden at launch by the WR64_TEXPACK "
            "environment variable.",
            "textures");
        tex_cfg.add_button_option("tex_browse_folder", "Browse for Pack Folder",
            "Pick the texture-pack folder with a file browser. Fills in the path "
            "above; press Apply to load it.",
            "Browse Folder…");
        tex_cfg.add_button_option("tex_browse_zip", "Browse for Pack .zip",
            "Pick a texture-pack .zip file with a file browser. Fills in the path "
            "above; press Apply to load it.",
            "Browse .zip…");
        tex_cfg.add_option_change_callback("tex_browse_folder",
            [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant,
               recomp::config::OptionChangeContext ctx) {
                if (ctx == recomp::config::OptionChangeContext::Load) return;
                recompui::file::open_folder_dialog([](bool success, const std::filesystem::path& path) {
                    if (success) {
                        recompui::config::get_config("wr64_textures").update_option_value(
                            "tex_pack_dir", path.string());
                        recompui::config::show_notification(recompui::config::NotificationType::Info,
                            "Pack folder selected — press Apply to load it.");
                    }
                });
            });
        tex_cfg.add_option_change_callback("tex_browse_zip",
            [](recomp::config::ConfigValueVariant, recomp::config::ConfigValueVariant,
               recomp::config::OptionChangeContext ctx) {
                if (ctx == recomp::config::OptionChangeContext::Load) return;
                recompui::file::open_file_dialog([](bool success, const std::filesystem::path& path) {
                    if (success) {
                        recompui::config::get_config("wr64_textures").update_option_value(
                            "tex_pack_dir", path.string());
                        recompui::config::show_notification(recompui::config::NotificationType::Info,
                            "Pack .zip selected — press Apply to load it.");
                    }
                });
            });
        tex_cfg.add_bool_option("tex_dump", "Dump Textures (advanced)",
            "Write every texture the game loads to the 'textures_dump' folder "
            "(raw N64 format). Decode them into editable PNGs + a loadable pack "
            "with scripts/decode_texture_dump.py — see textures/README.md. For "
            "creating packs; leave off for normal play.",
            false);

        auto apply_tex = []() {
            recomp::config::Config& cfg = recompui::config::get_config("wr64_textures");
            wr64_set_texture_pack(std::get<std::string>(cfg.get_option_value("tex_pack_dir")).c_str());
            wr64_set_texture_replace(std::get<bool>(cfg.get_option_value("tex_enable")));
            wr64_set_texture_dump(std::get<bool>(cfg.get_option_value("tex_dump")));
        };
        tex_cfg.set_load_callback(apply_tex);
        tex_cfg.set_save_callback(apply_tex);
    }

    // Mods tab: texture packs (.rtz containers, Zelda64Recomp-style). The
    // content type below keys on rt64.json, so any pack dropped into mods/
    // shows up here with a per-pack toggle.
    recompui::config::create_mods_tab();

    recompui::config::finalize();

    // Wave Race 64 is single-player: use the SP keyboard + controller profiles
    // directly so input works without going through the player-assignment modal.
    // get_n64_input() (see src/input.cpp) reads these profiles.
    recompinput::players::set_single_player_mode(true);

    // Texture-pack mod support (Zelda64Recomp pattern): a mod containing an
    // rt64.json is a texture pack; the .rtz container extension wraps a pack
    // zip with no manifest required (one is auto-created from the filename).
    // Enabled packs are staged via wr64_mod_texture_pack_* and applied on the
    // gfx thread together with the Textures-tab pack.
    recomp::mods::ModContentType texture_pack_content_type{
        .content_filename = "rt64.json",
        .allow_runtime_toggle = true,
        .on_enabled = [](recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
            wr64_mod_texture_pack_enabled(mod.manifest.mod_id);
        },
        .on_disabled = [](recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
            wr64_mod_texture_pack_disabled(mod.manifest.mod_id);
        },
        .on_reordered = [](recomp::mods::ModContext&) {
            wr64_mod_texture_packs_reordered();
        },
    };
    auto texture_pack_content_type_id = recomp::mods::register_mod_content_type(texture_pack_content_type);
    recomp::mods::register_mod_container_type("rtz", std::vector{ texture_pack_content_type_id }, false);

    // Pause-for-menu disabled pending fix (menu input freezes when paused).
    // recompui::config::set_menu_close_callback([]() {
    //     ultramodern::set_paused_for_menu(false);
    // });

    recompui::register_launcher_init_callback([](recompui::LauncherMenu* menu) {
        using namespace recompui;
        auto* options = menu->init_game_options_menu(
            u8"waverace64",
            "waverace64",
            "Wave Race 64",
            {},
            GameOptionsMenuLayout::Center
        );
        // Original wave-themed launcher backdrop (assets/wr64_background.svg).
        // Not the copyrighted Nintendo logo/artwork — an original evocation.
        menu->set_launcher_background_svg("wr64_background.svg");

        // Original wordmark logo (assets/wr64_logo.svg — a Pillow-rendered water
        // gradient wordmark, our own design/effects with the bundled Lato font;
        // not the game's trademarked logo or font). Replaces the plain title.
        ContextId ctx = get_launcher_context_id();
        menu->remove_default_title();
        Svg* logo = ctx.create_element<Svg>(menu, "wr64_logo.svg");
        logo->set_position(Position::Absolute);
        logo->set_top(23.0f, Unit::Percent);
        logo->set_left(50.0f, Unit::Percent);
        logo->set_translate_2D(-50.0f, -50.0f, Unit::Percent);
        logo->set_width(560.0f);   // aspect ~4.7:1 (1467x312, framed plate)
        logo->set_height(119.0f);

        // Nudge the whole options list up a little (Center layout anchors it by
        // bottom%, so raise that — do NOT set_translate_2D here, that would clobber
        // the layout's -50% X centering and shove the list to the right).
        options->set_bottom(30.0f, Unit::Percent);

        // Add options individually (instead of add_default_options()) to
        // brighten the resting text so it reads clearly over the background
        // art (the default is theme TextDim, which was hard to see).
        GameOption* opts[] = {
            options->add_start_game_or_load_rom_option(),
            options->add_mods_option(),
            options->add_setup_controls_option(),
            options->add_settings_option(),
            options->add_exit_option(),
        };
        for (GameOption* o : opts) {
            o->set_color(theme::color::White);
        }
    });
#endif

    recomp::start(config);

    // Cleanup
#ifdef HAS_RECOMPUI
    NFD_Quit();
#endif
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    SDL_Quit();

    printf("[WR64] Exited cleanly.\n");
    return 0;
}
