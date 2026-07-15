/**
 * @file audio.cpp
 * @brief SDL2-based audio callback implementation for WaveRace64-Recomp.
 *
 * Uses SDL2's audio queue API for low-latency audio playback.
 * The N64 audio thread submits buffers via audio_queue_samples(),
 * and SDL2 drains them through its audio device callback.
 */

#include "audio.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <mutex>
#include <SDL2/SDL.h>

namespace wr64 {

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

static SDL_AudioDeviceID audio_device = 0;
static uint32_t current_frequency = 32000;
// Rate the SDL device is actually open at. Tracked separately from
// current_frequency: comparing against current_frequency in
// ensure_audio_device() after set_frequency() already updated it meant the
// device was never reopened on a rate change — audio then played at the
// startup dummy rate (48000) regardless of what the game requested,
// causing fast/high-pitched output and queue-underrun crackle.
static uint32_t device_frequency = 0;
static std::atomic<size_t> queued_samples{0};

// ---------------------------------------------------------------------------
// SDL audio device management
// ---------------------------------------------------------------------------

static void ensure_audio_device(uint32_t freq) {
    if (audio_device != 0 && device_frequency == freq) {
        return; // Already open at the correct frequency.
    }

    // Close existing device if frequency changed.
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }

    SDL_AudioSpec desired{};
    desired.freq     = static_cast<int>(freq);
    desired.format   = AUDIO_S16SYS;
    desired.channels = 2;       // Stereo
    desired.samples  = 512;     // Buffer size in samples per channel
    desired.callback = nullptr; // Use SDL_QueueAudio instead of callback

    SDL_AudioSpec obtained{};
    audio_device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (audio_device == 0) {
        fprintf(stderr, "[WR64-Audio] SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return;
    }

    device_frequency = freq;
    queued_samples.store(0);

    // Unpause the device to start playback.
    SDL_PauseAudioDevice(audio_device, 0);

    printf("[WR64-Audio] Audio device opened: freq=%d, channels=%d, samples=%d\n",
           obtained.freq, obtained.channels, obtained.samples);
}

// ---------------------------------------------------------------------------
// Public API (matches ultramodern::audio_callbacks_t)
// ---------------------------------------------------------------------------

void audio_queue_samples(int16_t* samples, size_t count) {
    ensure_audio_device(current_frequency);

    if (audio_device == 0 || samples == nullptr || count == 0) {
        return;
    }

    // count is the number of int16_t values (so byte count = count * 2).
    size_t byte_count = count * sizeof(int16_t);
    if (SDL_QueueAudio(audio_device, samples, static_cast<uint32_t>(byte_count)) != 0) {
        fprintf(stderr, "[WR64-Audio] SDL_QueueAudio failed: %s\n", SDL_GetError());
        return;
    }

    queued_samples.fetch_add(count);
}

size_t audio_get_frames_remaining() {
    if (audio_device == 0) {
        return 0;
    }

    // SDL_GetQueuedAudioSize returns bytes. The runtime expects FRAMES
    // (ultramodern multiplies by 2 channels * sizeof(int16_t)); reporting
    // samples here doubles the apparent buffer level, causing underruns and
    // audible static.
    constexpr uint32_t bytes_per_frame = 2 * sizeof(int16_t); // stereo s16
    uint64_t buffered_frames = SDL_GetQueuedAudioSize(audio_device) / bytes_per_frame;

    // Report one VI-refresh of frames fewer than actually buffered so the game
    // generates audio slightly ahead of playback. Same technique as
    // Pilotwings64Recomp; prevents popping when the audio thread lags.
    uint32_t frames_per_vi = current_frequency / 60;
    if (buffered_frames > frames_per_vi) {
        buffered_frames -= frames_per_vi;
    }
    else {
        buffered_frames = 0;
    }

    return static_cast<size_t>(buffered_frames);
}

void audio_set_frequency(uint32_t freq) {
    if (freq == 0) {
        fprintf(stderr, "[WR64-Audio] Ignoring zero frequency\n");
        return;
    }
    current_frequency = freq;
    ensure_audio_device(freq);
}

} // namespace wr64
