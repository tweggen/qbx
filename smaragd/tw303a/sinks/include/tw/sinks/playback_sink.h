#ifndef PLAYBACK_SINK_H
#define PLAYBACK_SINK_H

#include <cstddef>
#include <memory>
#include <atomic>
#include <cstdint>

#include "tw/sinks/audio_sink.h"

namespace audio {

class AudioBackend;

/**
 * PlaybackSink: Real-time audio output to device.
 *
 * Streams frames directly to the audio backend's callback interface.
 * Non-buffered: frames flow immediately to the device with minimal latency.
 *
 * Thread-safe: writeFrames() can be called from the audio callback thread.
 *
 * Integrated with twspeaker.cc for Phase 5b unification.
 */
class PlaybackSink : public AudioSink {
public:
    /**
     * Create a playback sink for the given audio backend.
     *
     * \param backend  Audio backend (WASAPI, CoreAudio, ALSA, etc.)
     */
    explicit PlaybackSink(AudioBackend* backend);
    ~PlaybackSink() override = default;

    /**
     * Write a block of interleaved frames to the device output buffer.
     *
     * Non-blocking: frames are queued for the realtime callback.
     * If the callback can't keep up, frames may be dropped (underrun).
     *
     * \param interleaved  nFrames * channels floats, channel-minor
     * \param nFrames      Frames in the block
     * \param channels     Channels per frame
     * \return             Always true (accepts all frames, may drop on underrun)
     */
    bool writeFrames(const float *interleaved, std::size_t nFrames,
                     unsigned channels) override;

    /**
     * Flush any buffered data (no-op for realtime playback).
     *
     * Real-time playback has no intermediate buffer to flush.
     * Frames flow directly from AudioEngine → callback → device.
     */
    void flush() override {}

    /**
     * Get sink name for diagnostics.
     */
    const char* name() const override { return "PlaybackSink"; }

private:
    AudioBackend* backend_;  // Unused in Phase 5b; kept for future enhancement
};

}  // namespace audio

#endif  // PLAYBACK_SINK_H
