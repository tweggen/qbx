#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <cstdint>
#include <memory>
#include <vector>
#include <atomic>

#include "../twresampler.h"

class twComponent;
class twLatchStreamingOutput;

namespace audio {

// Universal audio frame (L and R channels)
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

/**
 * AudioEngine: Unified audio pull from component graph.
 *
 * Replaces duplicated pull logic in twspeaker.cc and render_session.cc.
 * Handles:
 * - Component graph traversal and resampling
 * - Position management (seeking, playhead tracking)
 * - Stereo wiring (L/R channel synthesis)
 * - Loop cycling (for playback)
 *
 * Both playback and render instantiate one AudioEngine and call
 * pullFrame() in their respective loops.
 */
class AudioEngine {
public:
    /**
     * Create an engine for the given synth component.
     *
     * \param synthOutput Component to pull audio from (e.g., rewire root)
     * \param sampleRate  Sample rate for the engine
     */
    AudioEngine(twComponent* synthOutput, uint32_t sampleRate);
    ~AudioEngine();

    /**
     * Pull one stereo audio frame from the component graph.
     *
     * Handles resampling, position tracking, and L/R channel wiring.
     * Safe to call from realtime thread (callback) or render thread.
     *
     * \param outFrame  Output: stereo frame (L, R channels)
     * \return          True if a valid frame was produced; false on error/underrun
     */
    bool pullFrame(AudioFrame& outFrame);

    /**
     * Seek to an absolute position in the component graph.
     *
     * Called before rendering a time range, or during playback
     * when cycling through a loop region.
     *
     * \param offsetSamples  Absolute position in samples
     */
    void seekTo(uint64_t offsetSamples);

    /**
     * Get the current playback position.
     * Lock-free (safe to call from any thread).
     */
    uint64_t currentPosition() const;

    /**
     * Set loop boundaries and enable/disable looping.
     *
     * \param enabled   Whether to loop
     * \param start     Loop start position (samples)
     * \param end       Loop end position (samples)
     */
    void setLoopBoundaries(bool enabled, uint64_t start, uint64_t end);

    /**
     * Configure the resampler sample rate.
     * Called after device open or when sample rate changes.
     *
     * \param inRate   Input sample rate (component graph rate)
     * \param outRate  Output sample rate (device or render rate)
     */
    void configureResampling(uint32_t inRate, uint32_t outRate);

private:
    twComponent* synthOutput_;
    uint32_t engineSampleRate_;  // The engine's native sample rate

    // Resampling
    twResampler resamplerL_;
    twResampler resamplerR_;
    double rateRatio_;  // output / input sample rate

    // Loop state (atomic for lockfree access)
    std::atomic<bool> cycleEnabled_{false};
    std::atomic<uint64_t> loopStart_{0};
    std::atomic<uint64_t> loopEnd_{0};
    std::atomic<uint64_t> currentPos_{0};

    // Helper: pull one frame of L and R at engine sample rate
    bool pullStereoFrame(float& outL, float& outR);
};

}  // namespace audio

#endif  // AUDIO_ENGINE_H
