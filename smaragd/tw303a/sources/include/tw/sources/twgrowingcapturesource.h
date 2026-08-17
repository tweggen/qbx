#ifndef _TWGROWINGCAPTURESOURCE_H_
#define _TWGROWINGCAPTURESOURCE_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "tw/sources/twrandomsource.h"

class twCapturingSource;

/**
 * A capture whose length GROWS while it is being read (proposal 21 L3a,
 * design D7).
 *
 * twCapturingSource is a SNAPSHOT: it is filled once, in its constructor, and
 * its length is fixed from that instant. That is exactly right for a container
 * or asset capture and exactly wrong for a recording, whose whole point is
 * that the material arrives over time and the arranger draws, previews and
 * eventually plays back a clip that is still getting longer. This is the
 * second shape, and the two coexist deliberately — nothing about the fixed one
 * changes.
 *
 * Storage: CHUNKED PLANAR. One chunk is `chunkFrames` frames of every channel
 * (channel c at `c * chunkFrames` floats inside the chunk), the default being
 * twOutputPage::FRAME_CAPACITY so a chunk is exactly one page of one channel.
 * Chunked rather than one growing vector for the reason that matters here: a
 * std::vector that reallocates MOVES the samples a concurrent reader is in the
 * middle of copying out. A chunk, once published, never moves and is never
 * freed until the whole source dies.
 *
 * Threading
 *   - ONE producer thread calls append()/reserveThrough(). It is the only
 *     writer, the only allocator, and the only writer of frontier_.
 *   - ANY number of reader threads call read()/readInterleaved()/frontier().
 *     A read past the frontier is a SHORT READ that zero-fills the tail; it
 *     never waits, never allocates and never takes a lock. A live source has
 *     no "not yet" answer to give a reader, and a reader that blocked on one
 *     would be a reader that could deadlock the audio thread.
 *   - The publication is frontier_'s release store: everything the producer
 *     wrote (samples AND the chunk pointer that holds them) happens-before a
 *     reader's acquire load of the frontier, and a reader only ever touches
 *     frames below the frontier it loaded. That single pairing is the whole
 *     synchronisation.
 *
 * Allocation: one chunk (`channels * chunkFrames * 4` bytes — 512 KB for a
 * stereo default chunk) is allocated by the PRODUCER, inside append(), when
 * the frontier crosses a chunk boundary: at 48 kHz stereo that is one malloc
 * every 1.37 s. A producer that may not allocate at all can pre-allocate with
 * reserveThrough() while it is still allowed to. The chunk INDEX never
 * reallocates: it is a fixed array of atomic pointers sized at construction,
 * so publishing a chunk cannot move the index a reader is walking. Appending
 * past its end is refused and counted in droppedFrames() — silently growing
 * without bound is not a better failure.
 */
class twGrowingCaptureSource
    : public twRandomSource
{
public:
    // One chunk = one twOutputPage's worth of ONE channel. Kept as a plain
    // constant rather than an include of tw/pages so this header stays cheap;
    // the .cc static_asserts it against twOutputPage::FRAME_CAPACITY.
    static constexpr std::size_t kDefaultChunkFrames = 65536;

    // 4096 chunks of the default size is 268 M frames — 1.55 hours at 48 kHz —
    // for a 32 KB index. Beyond that append() refuses.
    static constexpr std::size_t kDefaultMaxChunks = 4096;

    twGrowingCaptureSource( idx_t channels, int sampleRate,
                            std::size_t chunkFrames = kDefaultChunkFrames,
                            std::size_t maxChunks   = kDefaultMaxChunks );
    ~twGrowingCaptureSource() override;

    // ---- producer side (ONE thread) ------------------------------------

    /// Append `frames` INTERLEAVED frames of channels() channels. Returns the
    /// number of frames actually stored; a shortfall (index exhausted, or an
    /// allocation that failed) is counted in droppedFrames().
    std::size_t append( const float *interleaved, std::size_t frames );

    /// Append `frames` frames given as `channels()` PLANAR pointers.
    std::size_t appendPlanar( const float *const *planar, std::size_t frames );

    /// Pre-allocate the chunks covering [0, frames). Lets a producer that must
    /// not allocate in its steady state pay for the storage up front.
    /// Returns the number of frames now backed by allocated storage.
    std::size_t reserveThrough( std::size_t frames );

    // ---- anyone ---------------------------------------------------------

    /// Frames appended so far. Monotone; the only thing a reader may trust.
    std::uint64_t frontier() const
    { return frontier_.load( std::memory_order_acquire ); }

    std::uint64_t droppedFrames() const
    { return dropped_.load( std::memory_order_relaxed ); }

    std::size_t chunkFrames() const { return chunkFrames_; }
    std::size_t chunksAllocated() const
    { return chunksAllocated_.load( std::memory_order_relaxed ); }
    std::size_t residentBytes() const;

    // ---- reader side (any thread) ---------------------------------------

    /// twRandomSource: stateless planar read of one channel. Frames at or past
    /// the frontier are zero-filled; the return value is the number of frames
    /// actually backed by captured audio.
    length_t read( offset_t srcOffset, sample_t *dest,
                   length_t len, idx_t channel ) const override;

    /// Interleaved read for a file writer. `channelMask` selects channels
    /// (bit n = channel n; 0 = all), so one growing capture can feed several
    /// files of different widths without a second copy of the audio. `dest`
    /// must hold `frames * maskedChannels(channelMask)` floats. Returns the
    /// number of frames actually backed by captured audio (the rest is zeroed).
    std::size_t readInterleaved( std::uint64_t startFrame, float *dest,
                                 std::size_t frames,
                                 std::uint32_t channelMask ) const;

    /// How many channels `channelMask` selects out of channels().
    std::uint32_t maskedChannels( std::uint32_t channelMask ) const;

    length_t length()     const override { return (length_t) frontier(); }
    idx_t    channels()   const override { return channels_; }
    int      sampleRate() const override { return sampleRate_; }
    bool     isReproducible() const override { return false; }

    /// Hand the captured storage to a FIXED-SIZE source, with exactly ONE copy
    /// of the audio: the flat planar buffer twCapturingSource adopts is built
    /// straight out of the chunks and moved in. `frames < 0` means "everything
    /// up to the frontier".
    std::shared_ptr<twCapturingSource> toCapturingSource(
            std::uint64_t startFrame = 0, length_t frames = -1 ) const;

private:
    // Returns the chunk holding `frame`, allocating it if needed. Producer
    // only. Null when the index is exhausted or the allocation threw.
    float *chunkForWrite_( std::uint64_t frame );
    // Reader side: the published chunk holding `frame`, or null.
    const float *chunkForRead_( std::uint64_t frame ) const;

    idx_t       channels_;
    int         sampleRate_;
    std::size_t chunkFrames_;

    // Fixed-size index of published chunks. Sized once, never resized: a
    // reader walks it concurrently with the producer publishing into it.
    std::vector<std::atomic<float *> > chunks_;

    std::atomic<std::uint64_t> frontier_{ 0 };
    std::atomic<std::uint64_t> dropped_{ 0 };
    std::atomic<std::size_t>   chunksAllocated_{ 0 };
};

#endif
