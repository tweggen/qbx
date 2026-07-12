#ifndef AUDIO_SINK_H
#define AUDIO_SINK_H

#include <memory>

namespace audio {
}

#include "tw/core/audio_frame.h"

namespace audio {

/**
 * AudioSink: Abstract base for audio output destinations.
 *
 * Decouples the audio engine from specific output targets.
 * Both playback (device) and render (file) use the same sink interface.
 *
 * Sinks are non-blocking: writeFrame() queues/buffers data and returns
 * immediately. FileSink handles buffering and disk writes internally.
 */
class AudioSink {
public:
    virtual ~AudioSink() = default;

    /**
     * Write one stereo audio frame to the sink.
     *
     * Implementation may:
     * - Queue the frame for output (playback sink)
     * - Buffer it (file sink, waiting for completeness)
     * - Drop it (network sink under load)
     *
     * Must be thread-safe (called from audio callback or render thread).
     *
     * \param frame  Stereo audio frame to write
     * \return       True if frame was accepted; false on error/overflow
     */
    virtual bool writeFrame(const AudioFrame& frame) = 0;

    /**
     * Flush any buffered/queued data.
     *
     * Playback sink: no-op (real-time output has no buffer to flush).
     * File sink: write all buffered frames to disk and close.
     * Network sink: send any pending packets.
     *
     * Called when stopping or finalizing a render.
     */
    virtual void flush() = 0;

    /**
     * Get human-readable sink description (for diagnostics).
     */
    virtual const char* name() const = 0;
};

/**
 * PlaybackSink: Real-time audio output to device.
 *
 * Frames flow immediately to the audio backend with no buffering.
 * If the render callback can't keep up (underrun), frames are dropped.
 *
 * Not implemented in Phase 5a; twspeaker.cc remains the playback sink
 * until Phase 5b unifies the playback path.
 */
// class PlaybackSink : public AudioSink { ... };  // Phase 5b

/**
 * FileSink: Buffered audio output to file with futures-based completion.
 *
 * Buffers incoming frames and waits for:
 * 1. Revalidation completeness (future resolved), OR
 * 2. Data age timeout (500ms, prevents forever-wait on stale captures)
 *
 * Then writes complete buffers to disk via AudioFileWriter.
 *
 * Integrated in Phase 5c.
 */
// class FileSink : public AudioSink { ... };  // Phase 5c

}  // namespace audio

#endif  // AUDIO_SINK_H
