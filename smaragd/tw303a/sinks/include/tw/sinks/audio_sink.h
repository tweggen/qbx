#ifndef AUDIO_SINK_H
#define AUDIO_SINK_H

#include <cstddef>
#include <memory>

namespace audio {

/**
 * AudioSink: Abstract base for audio output destinations.
 *
 * Decouples the audio engine from specific output targets.
 * Both playback (device) and render (file) use the same sink interface.
 *
 * Sinks are non-blocking: writeFrames() queues/buffers data and returns
 * immediately. FileSink handles buffering and disk writes internally.
 *
 * WIDTH (proposal 36 B5). A sink is N-channel and the width is carried by the
 * CALL, not by a fixed frame type. It used to be an `AudioFrame` — a struct
 * with `float channels[MAX_CHANNELS]`, `MAX_CHANNELS == 2` — passed one frame
 * per call, which put a hard stereo cap in `tw/core` and made the sink the last
 * place in the engine that could not represent what the graph produces. The
 * interface is a BLOCK of interleaved frames instead: it carries any width, and
 * it is one call per block rather than one per frame.
 */
class AudioSink {
public:
    virtual ~AudioSink() = default;

    /**
     * Write a block of INTERLEAVED frames to the sink.
     *
     * Implementation may:
     * - Queue the block for output (playback sink)
     * - Buffer it (file sink, waiting for completeness)
     * - Drop it (network sink under load)
     *
     * Must be thread-safe (called from audio callback or render thread).
     *
     * \param interleaved  nFrames * channels floats, channel-minor
     * \param nFrames      Frames in the block
     * \param channels     Channels per frame; must match what the sink's
     *                     writer was opened with
     * \return             True if the block was accepted; false on error
     */
    virtual bool writeFrames(const float *interleaved, std::size_t nFrames,
                             unsigned channels) = 0;

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
    virtual const char *name() const = 0;
};

}  // namespace audio

#endif  // AUDIO_SINK_H
