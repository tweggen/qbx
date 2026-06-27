#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <cstdint>
#include <memory>
#include <vector>
#include <atomic>

#include "../twresampler.h"

class twComponent;
class twLatchStreamingOutput;
class twOutputPage;

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
 * AudioEngine: Unified audio pull from component graph with sequential frozen pages.
 *
 * Tier 1 Enhancement: Playback Path Refactoring
 * Uses sequential freezePage() rendering (like export) instead of seekTo(),
 * preserving component state during playback looping.
 *
 * Solves: Reverb/delay state loss during playback (correctness issue)
 *
 * Replaces duplicated pull logic in twspeaker.cc and render_session.cc.
 * Handles:
 * - Component graph traversal with frozen pages (no seekTo corruption)
 * - Position management (playhead tracking, loop cycling)
 * - Stereo wiring (L/R channel synthesis)
 * - Loop cycling with state preservation
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
     * Uses frozen pages for state-continuous rendering.
     * Safe to call from realtime thread (callback) or render thread.
     *
     * \param outFrame  Output: stereo frame (L, R channels)
     * \return          True if a valid frame was produced; false on error/underrun
     */
    bool pullFrame(AudioFrame& outFrame);

    /**
     * Pull a block of stereo audio frames from the component graph.
     *
     * More efficient than calling pullFrame() repeatedly.
     * Safe to call from realtime thread (callback) or render thread.
     *
     * \param outL      Output: L channel samples (must hold at least nFrames floats)
     * \param outR      Output: R channel samples (must hold at least nFrames floats)
     * \param nFrames   Number of frames to pull
     * \return          Number of frames actually produced (0 on error)
     */
    length_t pullBlock(float* outL, float* outR, length_t nFrames);

    /**
     * Seek to an absolute position in the component graph.
     *
     * Called before rendering a time range, or during playback scrubbing.
     * Note: Seeking resets component state. Looping uses frozen pages instead
     * (via setLoopBoundaries) to preserve state.
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
     * Unlike seeking, looping uses sequential frozen pages to preserve state.
     * This ensures reverb/delay/grain components maintain state across loop wraps.
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

    // Frozen page rendering state (Tier 1 enhancement)
    // Maintains sequential frozen pages for state continuity during playback
    std::shared_ptr<twOutputPage> currentFrozenPage_;
    std::shared_ptr<twOutputPage> prevFrozenPage_;  // For state resumption
    uint64_t currentPageStartPos_;
    size_t pageFrameOffset_;  // Current offset within page's sample buffer
    uint64_t currentPageGeneration_{0};  // Track page generation to detect invalidation

    // Resampling
    twResampler resamplerL_;
    twResampler resamplerR_;
    double rateRatio_;  // output / input sample rate

    // Loop state (atomic for lockfree access)
    std::atomic<bool> cycleEnabled_{false};
    std::atomic<uint64_t> loopStart_{0};
    std::atomic<uint64_t> loopEnd_{0};
    std::atomic<uint64_t> currentPos_{0};

    // Helper: pull one frame of L and R at engine sample rate using frozen pages
    bool pullStereoFrameFrozen(float& outL, float& outR);

    // Helper: manage frozen page transitions and state continuity
    void updateFrozenPage(uint64_t desiredPos);
};

}  // namespace audio

#endif  // AUDIO_ENGINE_H
