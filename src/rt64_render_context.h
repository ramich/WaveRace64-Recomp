/**
 * @file rt64_render_context.h
 * @brief RT64 RendererContext factory for WaveRace64-Recomp.
 *
 * Provides a factory function that creates an ultramodern::renderer::RendererContext
 * subclass backed by the RT64 rendering library.
 */

#ifndef WR64_RT64_RENDER_CONTEXT_H
#define WR64_RT64_RENDER_CONTEXT_H

#include <memory>
#include <cstdint>
#include "ultramodern/renderer_context.hpp"

namespace wr64 {

/**
 * Factory function matching ultramodern::renderer::callbacks_t::create_render_context_t.
 *
 * Creates and returns a RendererContext backed by RT64::Application.
 *
 * @param rdram          Pointer to the emulated RDRAM buffer.
 * @param window_handle  Platform window handle (SDL_Window* on Linux).
 * @param developer_mode If true, enable RT64 developer/debug features.
 * @return A unique_ptr to the created RendererContext, or nullptr on failure.
 */
std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(
    uint8_t* rdram,
    ultramodern::renderer::WindowHandle window_handle,
    bool developer_mode
);

/**
 * Forward an SDL event to RT64 (developer inspector input, F1-F4 shortcuts).
 * @param sdl_event Pointer to an SDL_Event (typed void* to keep SDL out of this header).
 * @return true if RT64 consumed the event.
 */
bool rt64_handle_sdl_event(void* sdl_event);

/**
 * Returns the number of game frames (display lists) submitted since the last
 * call, for FPS display. Thread-safe.
 */
uint32_t rt64_consume_frame_count();
// Presented frames (incl. interpolated) since last consumption — perceived FPS.
uint32_t rt64_consume_present_count();

// True once a WR64_TEXPACK replacement pack has been loaded this session.
bool rt64_texture_pack_loaded();
// Current state of RT64's texture-replacement toggle (F4).
bool rt64_replacements_enabled();

} // namespace wr64

#endif // WR64_RT64_RENDER_CONTEXT_H
