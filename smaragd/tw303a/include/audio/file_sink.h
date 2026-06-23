#ifndef FILE_SINK_H
#define FILE_SINK_H

#include <memory>
#include <deque>
#include <cstdint>
#include <chrono>
#include <future>
#include <mutex>

#include "audio_sink.h"

namespace audio {

class AudioFileWriter;
class GenerationPromise;

/**
 * FileSink: Buffered file output with futures-based completeness tracking.
 *
 * Buffers incoming AudioFrames and writes them to disk only when:
 * 1. Revalidation is complete (future resolved), OR
 * 2. Data is old enough (age-based fallback, prevents forever-wait)
 *
 * This is the render-specific sink (Phase 5c). Playback uses device callback directly.
 *
 * Thread-safe: writeFrame() and flush() can be called from render thread.
 */
class FileSink : public AudioSink {
public:
    static constexpr size_t DEFAULT_BUFFER_SIZE = 8192;  // Frames to buffer before flush
    static constexpr int64_t DEFAULT_AGE_TIMEOUT_MS = 500;  // Fallback: write if ≥500ms old

    /**
     * Create a file sink for buffered output.
     *
     * \param writer         AudioFileWriter to write frames to disk
     * \param bufferFrames   How many frames to buffer (default 8192)
     * \param ageTimeoutMs   Fallback: write stale frames after this age (default 500ms)
     */
    FileSink(AudioFileWriter* writer,
             size_t bufferFrames = DEFAULT_BUFFER_SIZE,
             int64_t ageTimeoutMs = DEFAULT_AGE_TIMEOUT_MS);

    ~FileSink() override = default;

    /**
     * Write one frame to the buffer (non-blocking).
     *
     * Frame is buffered with its generation ID and ready future.
     * If buffer is full, oldest frame is dropped (shouldn't happen in normal operation).
     *
     * \param frame  Audio frame to buffer
     * \return       True if frame was buffered; false on error
     */
    bool writeFrame(const AudioFrame& frame) override;

    /**
     * Flush buffered frames to disk.
     *
     * Writes all buffered frames that are ready (generation stable or aged).
     * Gives brief grace period (100ms) for in-flight futures to resolve.
     * Called at end of render to ensure all data written.
     */
    void flush() override;

    /**
     * Get sink name for diagnostics.
     */
    const char* name() const override { return "FileSink"; }

    /**
     * Set the generation ID for subsequent writes.
     *
     * All frames pushed after calling this will be tagged with this generation
     * and will wait for this generation's promise to resolve.
     *
     * \param generation  Generation ID for upcoming frames
     */
    void setGeneration(uint32_t generation);

    /**
     * Get current buffer occupancy (for diagnostics).
     */
    size_t occupancy() const;

private:
    struct FrameEntry {
        AudioFrame frame;
        uint32_t generation;
        std::shared_future<void> readyFuture;
        int64_t createdTimeMs;  // When frame was pushed (for age-based fallback)
    };

    AudioFileWriter* writer_;
    size_t maxBufferFrames_;
    int64_t ageTimeoutMs_;
    uint32_t currentGeneration_;

    mutable std::mutex bufferMutex_;
    std::deque<FrameEntry> buffer_;

    // Helpers
    int64_t getCurrentTimeMs() const;
    bool isFrameReady(const FrameEntry& entry) const;
    void flushReady();
};

}  // namespace audio

#endif  // FILE_SINK_H
