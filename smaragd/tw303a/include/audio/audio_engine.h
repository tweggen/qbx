#ifndef AUDIO_ENGINE_H
#define AUDIO_ENGINE_H

#include <cstdint>
#include <memory>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "../twresampler.h"

class twComponent;
class twLatchStreamingOutput;
struct twOutputPage;

namespace audio {

// Playback state for Phase 6b: minimum buffering before playback starts
enum class PlaybackState {
    STOPPED = 0,    // Not playing
    BUFFERING = 1,  // Readahead building initial buffer (3sec)
    PLAYING = 2     // Sufficient buffer; audio flowing
};

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

    /**
     * Start playback after buffering.
     * Phase 6b: Waits for readahead to build minimum 3-second buffer before transitioning to PLAYING.
     * Returns BUFFERING if not ready, PLAYING when buffer threshold met.
     *
     * \return Playback state: BUFFERING (still waiting) or PLAYING (ready)
     */
    PlaybackState startPlayback();

    /**
     * Get current playback state.
     * \return Current state: STOPPED, BUFFERING, or PLAYING
     */
    PlaybackState getPlaybackState() const;

    /**
     * Get the condition variable that signals when playback is ready.
     * Used by twSpeaker to wait until readahead has buffered enough.
     * \return Reference to playbackReadyCv
     */
    std::condition_variable& getPlaybackReadyCv() { return playbackReadyCv_; }

    // Read-ahead thread management
    void startReadahead();   // Start pre-computing pages in background
    void stopReadahead();    // Stop read-ahead thread

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
    uint32_t cachedPageValidFrames_{0};  // Phase 2 perf: Cache validFrames to avoid repeated loads

    // Resampling
    twResampler resamplerL_;
    twResampler resamplerR_;
    double rateRatio_;  // output / input sample rate
    std::vector<float> resampleBufL_;  // Pre-allocated resampling buffers (Phase 1 perf optimization)
    std::vector<float> resampleBufR_;  // Allocated once in configureResampling(), reused per block

    // Playback state management (Phase 6b: minimum buffering before playback)
    std::atomic<PlaybackState> playbackState_{PlaybackState::STOPPED};
    static constexpr uint64_t minBufferFrames_ = 144000;       // ~3 seconds at 48kHz (3 pages × 48000 frames)
    static constexpr uint64_t underrunThresholdFrames_ = 48000; // 1 second at 48kHz (Phase 6b confirmed)

    // Loop state (atomic for lockfree access)
    std::atomic<bool> cycleEnabled_{false};
    std::atomic<uint64_t> loopStart_{0};
    std::atomic<uint64_t> loopEnd_{0};
    std::atomic<uint64_t> currentPos_{0};

    // Helper: pull one frame of L and R at engine sample rate using frozen pages
    bool pullStereoFrameFrozen(float& outL, float& outR);

    // Helper: manage frozen page transitions and state continuity (read-only on audio thread)
    void updateFrozenPage(uint64_t desiredPos);
    // Read-ahead thread: pre-computes pages to keep ahead of playhead
    std::thread readaheadThread_;
    std::atomic<bool> readaheadRunning_{false};
    std::mutex readaheadMutex_;
    std::condition_variable readaheadCv_;
    std::condition_variable playbackReadyCv_;  // Signals twSpeaker when buffer ready for playback
    std::shared_ptr<twOutputPage> readaheadPrevPage_;   // State chain for read-ahead
    uint64_t readaheadComputedUpTo_{0};                 // Last page start pos computed

    void readaheadLoop();  // Entry point for read-ahead thread
};

}  // namespace audio

#endif  // AUDIO_ENGINE_H
