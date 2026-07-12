#ifndef _TW_CORE_AUDIO_FRAME_H_
#define _TW_CORE_AUDIO_FRAME_H_

#include <cstddef>
#include <cstdint>

// Universal audio frame (L and R channels). Shared currency between the
// playback engine (pullFrame) and the sinks (writeFrame) — lives in tw/core
// so neither module needs the other (proposal 14, Phase 2).
struct AudioFrame {
    static constexpr size_t MAX_CHANNELS = 2;
    float channels[MAX_CHANNELS];
    size_t numChannels;
    uint32_t sampleRate;

    AudioFrame() : numChannels(2), sampleRate(48000) {
        channels[0] = 0.0f;
        channels[1] = 0.0f;
    }

    AudioFrame(float l, float r, uint32_t sr = 48000)
        : numChannels(2), sampleRate(sr) {
        channels[0] = l;
        channels[1] = r;
    }
};

#endif
