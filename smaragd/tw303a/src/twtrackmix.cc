
//#include <qobjectlist.h>

#include <math.h>

#include "tw303aenv.h"
#include "twtrackmix.h"

int twTrackMix::seekTo( offset_t newOffset )
{
    std::lock_guard<std::mutex> lock(mutex());
    return seekTo_nolock(newOffset);
}

// Caller must hold mutex() (inherited from twComponent)
// CRITICAL: Lock protects clips_ iteration against UI thread modifications.
// Uses base class mutex to avoid introducing a second mutex (deadlock risk).
int twTrackMix::seekTo_nolock( offset_t newOffset )
{
    playOffset_.store( newOffset, std::memory_order_relaxed );

    // Propagate seek to all clips, computing their clip-relative offsets.
    // This ensures all child components are positioned correctly before the next
    // calcOutputTo call. In continuous forward play, the clip-relative offset will
    // match what calcOutputTo would compute anyway, so seekTo becomes a no-op for
    // reader cursors already at the right position. This design cleanly separates
    // concerns: "seek when position changes, advance on consecutive chunks."
    //
    // IMPORTANT: Seek ALL clips regardless of timeline position. If we only seek
    // clips within env.getBufferSize() of the seek point, clips starting later
    // retain stale reader positions (e.g., from prior playback), causing sparse/silent
    // audio when the render/playback reaches them.
    for( const ClipEntry &clip : clips_ ) {
        if( !clip.component ) {
            fprintf(stderr, "WARNING: twTrackMix::seekTo_nolock found null component in clips_\n");
            continue;
        }
        offset_t startTime = clip.startTime;
        // Seek this clip to the correct clip-relative position.
        // For clips not yet started (startTime > newOffset), this yields 0 (correct).
        // For clips already playing (startTime <= newOffset), this yields the offset
        // into the clip (also correct).
        offset_t clipRelative = std::max((offset_t)0, newOffset - startTime);
        clip.component->seekTo( clipRelative );
    }

    return 0;
}

idx_t twTrackMix::getNInputs() const
{
    return 0;
}

idx_t twTrackMix::getNOutputs() const
{
    return 1;
}
 
const char *twTrackMix::getInputName( idx_t ) const
{
    return NULL;
}

const char *twTrackMix::getOutputName( idx_t ) const
{
    return "Track bus sum";
}

bool twTrackMix::isSeekable() const
{
    return true;
}

void twTrackMix::insertClip(offset_t startTime, length_t duration, twComponent *component)
{
    std::lock_guard<std::mutex> lock(mutex());
    if( !component ) {
        fprintf(stderr, "ERROR: twTrackMix::insertClip received null component!\n");
        return;
    }
    clips_.push_back({startTime, duration, component});
    fprintf(stderr, "twTrackMix: inserted clip at time %llu, now have %zu clips\n", startTime, clips_.size());
}

void twTrackMix::removeClip(twComponent *component)
{
    std::lock_guard<std::mutex> lock(mutex());
    auto it = clips_.begin();
    int removed = 0;
    while( it != clips_.end() ) {
        if( it->component == component ) {
            it = clips_.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    fprintf(stderr, "twTrackMix: removed %d clip(s), now have %zu clips\n", removed, clips_.size());
}

void twTrackMix::updateClip(twComponent *component, offset_t newStartTime, length_t newDuration)
{
    std::lock_guard<std::mutex> lock(mutex());
    for( ClipEntry &clip : clips_ ) {
        if( clip.component == component ) {
            clip.startTime = newStartTime;
            clip.duration = newDuration;
            return;
        }
    }
}

void twTrackMix::setTrackMute(bool muted)
{
    std::lock_guard<std::mutex> lock(mutex());
    trackMuted_ = muted;
}

void twTrackMix::setTrackGain(double gainDb)
{
    std::lock_guard<std::mutex> lock(mutex());
    trackGainDb_ = gainDb;
}

void twTrackMix::createOutputLatches()
{
    pOutputLatches[0] = new twStreamingLatch( *this, 0, 0 );
}

void twTrackMix::setBufferSize( length_t )
{
    // NYI.
    return;
}

/**
 * Calc output to of a track mixer.
 * We scan for the given offset in the track, reading then data from the
 * objects directly into the buffer.
 * Currently we do not support mixing here, but should come.
 */
length_t twTrackMix::calcOutputTo( sample_t *buffer, length_t playLen, idx_t outChannel )
{
    std::lock_guard<std::mutex> lock(mutex());
    return calcOutputTo_nolock(buffer, playLen, outChannel);
}

// Caller must hold mutex() (inherited from twComponent)
// CRITICAL: Lock protects clips_ iteration against UI thread modifications.
// Uses base class mutex to avoid introducing a second mutex (deadlock risk).
length_t twTrackMix::calcOutputTo_nolock( sample_t *buffer, length_t playLen, idx_t outChannel )
{
    // FIXME: Why getBufferSize()? We have some length given!
    sample_t *readBuffer = (sample_t *) alloca( env.getBufferSize()*sizeof( sample_t ) );
    offset_t startInterval = playOffset_.load( std::memory_order_relaxed );
    offset_t endInterval   = startInterval + playLen;
    playOffset_.store( endInterval, std::memory_order_relaxed );

    memset( buffer, 0, sizeof( sample_t )*playLen );

    // FIXME: Take advantage of sorted clips_.
    for( const ClipEntry &clip : clips_ ) {
        offset_t startTime = clip.startTime;
        if( startTime>=endInterval ) continue;
        offset_t endTime = startTime;
        if( clip.duration > 0 ) {
            endTime += clip.duration;
            if( startInterval>=endTime ) continue;
        }
        // This clip is affected. Add the parts into the output buffer.
        offset_t startOffset;
        if( startTime<startInterval ) {
            startOffset = startInterval-startTime;
            startTime = startInterval;
        } else {
            startOffset = 0;
        }
        if( endTime ) {
            if( endTime>endInterval ) endTime = endInterval;
        } else {
            endTime = endInterval;
        }
        // Note: seekTo is now called once per position jump in twTrackMix::seekTo(),
        // not once per buffer. In continuous forward play, child readers are already
        // positioned correctly. This reduces seek calls from O(blocks) to O(seeks).
        if( !clip.component ) {
            fprintf(stderr, "WARNING: twTrackMix::calcOutputTo_nolock found null component in clips_\n");
            continue;
        }
        twComponent &cp = *clip.component;
        offset_t doRead = endTime-startTime;
        // Only zero out the range we'll actually use (not entire playLen)
        offset_t destOffset = startTime-startInterval;
        memset( readBuffer + destOffset, 0, sizeof( sample_t ) * doRead );
        // Get actual amount produced (may be less than doRead if component underruns)
        length_t actuallyGot = cp.calcOutputTo( readBuffer+destOffset, doRead, outChannel );
        // Only mix the actual samples produced (don't mix zero-padded tail)
        for( offset_t i = 0; i < actuallyGot; i++ ) {
            buffer[destOffset + i] += readBuffer[destOffset + i];
        }
    }

    // Intrinsic track processing: apply the track's own gain (and mute) here so
    // the output is self-contained — correct wherever it is summed, both by the
    // master mixer and, for groups, by a parent track. Volume is in dB (same
    // convention as twMixer::setInputLevel).
    double factor = trackMuted_ ? 0.0 : pow( 10., trackGainDb_/20. );
    if( factor != 1.0 ) {
        for( offset_t i=0; i<(offset_t)playLen; i++ ) {
            buffer[i] *= (sample_t) factor;
        }
    }
    return playLen;
}

// Phase 3: Freeze track output to a page
// Iterates clips that overlap [startPos, startPos+length), calls freezePage() on each
// child component, and mixes frozen outputs at correct timeline positions.
std::shared_ptr<twOutputPage> twTrackMix::freezePage(
    uint64_t startPos,
    const sample_t *inputData,
    uint64_t inputOffset,
    length_t inputLength,
    int sampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    std::lock_guard<std::mutex> lock(mutex());
    auto page = std::make_shared<twOutputPage>();
    page->startPosition = startPos;
    freezePage_nolock(page, startPos, inputLength, sampleRate, previousPage);
    return page;
}

// Caller must hold mutex()
length_t twTrackMix::freezePage_nolock(
    std::shared_ptr<twOutputPage> page,
    uint64_t startPos,
    length_t length,
    int sampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    // Initialize output buffer to silence
    std::fill(page->samples.begin(), page->samples.begin() + length, 0.0f);

    uint64_t endPos = startPos + length;

    // Iterate through clips that overlap [startPos, endPos)
    for( ClipEntry &clip : clips_ ) {  // Non-const to update previousPage
        uint64_t clipEnd = clip.startTime;
        if( clip.duration > 0 ) {
            clipEnd += clip.duration;
        } else {
            // Unbounded clip: treat as extending to page end
            clipEnd = endPos;
        }

        // Skip clips that don't overlap this page
        if( clip.startTime >= endPos || clipEnd <= startPos ) {
            continue;
        }

        // Freeze the child component's output for this range
        if( !clip.component ) {
            fprintf(stderr, "WARNING: twTrackMix::freezePage_nolock found null component in clips_\n");
            continue;
        }

        // Child position: what frame offset in the child corresponds to startPos?
        uint64_t childPos = (startPos >= clip.startTime)
                            ? (startPos - clip.startTime)
                            : 0;

        // Pass the clip's previous page so state carries forward across page boundaries
        // If this is the first page for this clip, previousPage will be nullptr (correct)
        auto childPage = clip.component->freezePage(
            childPos,
            nullptr,  // Track outputs don't consume input
            0,
            length,
            sampleRate,
            clip.previousPage  // Resume from previous page's state snapshot
        );

        if( !childPage || childPage->validFrames == 0 ) {
            continue;
        }

        // Save this page as the clip's previous page for the next page boundary
        clip.previousPage = childPage;

        // Mix child's frozen output into track output at correct timeline position
        uint64_t destOffset = (clip.startTime >= startPos)
                              ? (clip.startTime - startPos)
                              : 0;

        // Copy child's samples to output at the correct offset
        for( uint32_t i = 0; i < childPage->validFrames && destOffset + i < length; ++i ) {
            if( i < childPage->samples.size() ) {
                page->samples[destOffset + i] += childPage->samples[i];
            }
        }
    }

    // Apply track gain and mute (same as calcOutputTo_nolock)
    double factor = trackMuted_ ? 0.0 : pow( 10., trackGainDb_/20. );
    if( factor != 1.0 ) {
        for( size_t i = 0; i < length && i < page->samples.size(); ++i ) {
            page->samples[i] *= (sample_t) factor;
        }
    }

    page->validFrames = std::min((uint32_t)length, (uint32_t)page->samples.size());
    page->validAspects = twAspectPlayback;  // We've computed playback data
    return page->validFrames;
}

// Phase 3: Preview page freezing at lower resolution
// Delegates to base class freezePreviewPage() which calls freezePage() at preview rate
std::shared_ptr<twOutputPage> twTrackMix::freezePreviewPage(
    uint64_t startPos,
    length_t length,
    int previewSampleRate,
    int fullSampleRate,
    std::shared_ptr<twOutputPage> previousPage
)
{
    // Use base class implementation: calls freezePage() at preview resolution
    // Non-blocking: returns previousPage if new page not ready
    return twComponent::freezePreviewPage(startPos, length, previewSampleRate, fullSampleRate, previousPage);
}

twTrackMix::~twTrackMix()
{
}

twTrackMix::twTrackMix( tw303aEnvironment &env )
    : twComponent( env ),
      playOffset_( 0 ),
      trackMuted_( false ),
      trackGainDb_( 0.0 )
{
}


void twTrackMix::reset()
{
    // Reset play offset to zero
    playOffset_ = 0;
}
