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

#include "librecomp/game.hpp"
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
#include "nfd.h"

// WR64-specific renderer settings exposed by rt64_render_context.cpp.
extern void wr64_set_show_borders(bool show);
extern void wr64_set_wavegrid(uint32_t rows, uint32_t cols);
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

static void* create_gfx() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[WR64] SDL_Init failed: %s\n", SDL_GetError());
        return nullptr;
    }
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
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Give RT64 first look (developer inspector input, F1-F4 shortcuts).
        if (wr64::rt64_handle_sdl_event(&event)) {
            continue;
        }

#ifdef HAS_RECOMPUI
        // Forward every event to the UI system for RmlUi processing.
        recompui::queue_event(event);
#endif

        switch (event.type) {
            case SDL_QUIT:
                ultramodern::quit();
                break;
            case SDL_KEYDOWN:
#ifndef HAS_RECOMPUI
                if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                    ultramodern::quit();
                }
#endif
                break;
            default:
                break;
        }
    }

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

    // Title-bar FPS counter: game frames (display lists submitted) per second.
    static uint32_t last_ticks = 0;
    uint32_t now = SDL_GetTicks();
    if (now - last_ticks >= 1000) {
        uint32_t frames = wr64::rt64_consume_frame_count();
        if (last_ticks != 0 && window != nullptr) {
            char title[64];
            SDL_snprintf(title, sizeof(title), "Wave Race 64 - %.1f FPS",
                         frames * 1000.0f / (now - last_ticks));
            SDL_SetWindowTitle(window, title);
        }
        last_ticks = now;
    }
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
    recompui::config::create_mods_tab();

    // WR64-specific game settings tab.
    {
        recomp::config::Config& wr64_cfg = recompui::config::create_config_tab("WR64", "wr64_settings", false);
        wr64_cfg.add_bool_option("show_borders", "Show Borders",
            "Show the original CRT-overscan black borders. Off by default: the game renders edge-to-edge with a wider camera.",
            false);
        wr64_cfg.add_enum_option("wave_grid", "Wave Detail Area",
            "How far the detailed foam-water mesh extends into widescreen. Larger fills more of an ultrawide screen.",
            {
                {0u, "stock",  "Stock (19x35)"},
                {1u, "wide",   "Wide (23x55)"},
                {2u, "wider",  "Wider (28x70)"},
            }, 1u);

        auto apply_wr64 = []() {
            recomp::config::Config& cfg = recompui::config::get_config("wr64_settings");
            wr64_set_show_borders(std::get<bool>(cfg.get_option_value("show_borders")));
            static constexpr uint32_t rows[] = {19, 23, 28};
            static constexpr uint32_t cols[] = {35, 55, 70};
            uint32_t idx = std::get<uint32_t>(cfg.get_option_value("wave_grid"));
            if (idx >= 3) idx = 1;
            wr64_set_wavegrid(rows[idx], cols[idx]);
        };
        wr64_cfg.set_load_callback(apply_wr64);
        wr64_cfg.set_save_callback(apply_wr64);
    }

    recompui::config::finalize();

    recompui::register_launcher_init_callback([](recompui::LauncherMenu* menu) {
        auto* options = menu->init_game_options_menu(
            u8"waverace64",
            "waverace64",
            "Wave Race 64",
            {},
            recompui::GameOptionsMenuLayout::Center
        );
        options->add_default_options();
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
