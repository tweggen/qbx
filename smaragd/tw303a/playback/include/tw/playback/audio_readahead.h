#ifndef AUDIO_READAHEAD_H
#define AUDIO_READAHEAD_H

#include <cstdint>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "tw/playback/audio_engine.h"

namespace audio {

/**
 * AudioReadaheadBuffer: Ring buffer for smoothing async capture delays.
 *
 * Decouples the component graph pull (producer) from the audio sink
 * (consumer). Tolerates stale or incomplete captures by buffering ahead.
 *
 * Used by both playback (real-time smoothing) and render (lookahead
 * for disk writing).
 *
 * Non-blocking from the consumer side (playback/render waits briefly
 * if buffer is empty, then continues with stale data from Phase 4
 * async captures).
 */
class AudioReadaheadBuffer {
public:
    static constexpr size_t DEFAULT_READAHEAD_FRAMES = 8192;  // ~170ms @ 48kHz

    /**
     * Create a readahead buffer for the given sample rate.
     *
     * \param sampleRate      Audio sample rate (used for timeout calculations)
     * \param readaheadFrames Max frames to buffer (default 8192 = ~170ms)
     */
    AudioReadaheadBuffer(uint32_t sampleRate,
                        size_t readaheadFrames = DEFAULT_READAHEAD_FRAMES);

    ~AudioReadaheadBuffer();

    /**
     * Pull one frame from the buffer (consumer).
     *
     * Blocks briefly if buffer is empty (up to 5ms), then returns
     * whatever is available (may be stale from Phase 4 async captures).
     *
     * Called from playback callback or render thread.
     *
     * \param outFrame  Output: frame to consume
     * \return          True if frame was valid; false on buffer underrun
     */
    bool pullFrame(AudioFrame& outFrame);

    /**
     * Push one frame into the buffer (producer).
     *
     * Called from audio engine's pull loop. Non-blocking; if buffer
     * is full, drops the oldest frame.
     *
     * \param frame  Frame to buffer
     * \return       True if frame was enqueued; false if buffer full (dropped)
     */
    bool pushFrame(const AudioFrame& frame);

    /**
     * Get current buffer occupancy (for diagnostics).
     */
    size_t occupancy() const;

    /**
     * Reset buffer (discard all buffered frames, wake any waiters).
     *
     * Called during seek or when stopping playback.
     */
    void reset();

private:
    size_t maxFrames_;

    mutable std::mutex bufferMutex_;
    std::condition_variable bufferNotEmpty_;
    std::deque<AudioFrame> buffer_;

    // Diagnostics
    std::atomic<size_t> droppedFrames_{0};
};

}  // namespace audio

#endif  // AUDIO_READAHEAD_H
