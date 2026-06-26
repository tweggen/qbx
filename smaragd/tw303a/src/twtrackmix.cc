
//#include <qobjectlist.h>

#include <math.h>

#include "tw303aenv.h"
#include "twtrackmix.h"

#include "sobject.h"
#include "slink.h"
#include "strack.h"
#include "sstdmixer.h"

int twTrackMix::seekTo( offset_t newOffset )
{
    fprintf(stderr, "[twTrackMix::seekTo] Setting playOffset_=%ld\n", (long)newOffset);
    playOffset_.store( newOffset, std::memory_order_relaxed );
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

STrack &twTrackMix::getTrack() const
{
    return track_;
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
    // FIXME: Why getBufferSize()? We have some length given!
    sample_t *readBuffer = (sample_t *) alloca( env.getBufferSize()*sizeof( sample_t ) );
    offset_t startInterval = playOffset_.load( std::memory_order_relaxed );
    offset_t endInterval   = startInterval + playLen;
    playOffset_.store( endInterval, std::memory_order_relaxed );

    fprintf(stderr, "[twTrackMix::calcOutputTo] startInterval=%ld, endInterval=%ld, playLen=%ld\n",
            (long)startInterval, (long)endInterval, (long)playLen);

//      qWarning( "twTrackMix::calcOutputTo(): Called. track_=$%08x; playOffset_ = %d.\n",
//                &track_, playOffset_ );
    memset( buffer, 0, sizeof( sample_t )*playLen );

    int childCount = 0;
    // FIXME: Take advantage of sorted objects.
    for( SLink *lk : track_.childLinks() ) {
        childCount++;
        if( !lk->hasStartTime() ) {
            fprintf(stderr, "[Mixer] Child %d: no startTime, skipping\n", childCount);
            continue;
        }
        offset_t startTime = lk->getStartTime();
        fprintf(stderr, "[Mixer] Child %d: startTime=%ld, checking range [%ld, %ld)\n",
                childCount, (long)startTime, (long)startInterval, (long)endInterval);
        if( startTime>=endInterval ) {
            fprintf(stderr, "[Mixer] Child %d: startTime >= endInterval, skipping\n", childCount);
            continue;
        }
        offset_t endTime = startTime;
        if( lk->getSObject().hasDuration() ) {
            endTime += lk->getSObject().getDuration();
            fprintf(stderr, "[Mixer] Child %d: has duration, endTime=%ld\n", childCount, (long)endTime);
            if( startInterval>=endTime ) {
                fprintf(stderr, "[Mixer] Child %d: startInterval >= endTime, skipping\n", childCount);
                continue;
            }
        }
        // This object is affected. Add the parts into the output buffer.
        offset_t startOffset;
        if( startTime<startInterval ) {
            startOffset = startInterval-startTime;
            startTime = startInterval;
            fprintf(stderr, "[Mixer] Child %d: adjusted startOffset=%ld (started before interval)\n",
                    childCount, (long)startOffset);
        } else {
            startOffset = 0;
            fprintf(stderr, "[Mixer] Child %d: startOffset=0 (starts within interval)\n", childCount);
        }
        if( endTime ) {
            if( endTime>endInterval ) endTime = endInterval;
        } else {
            endTime = endInterval;
        }
        fprintf(stderr, "[Mixer] Child %d: SEEKING with startOffset=%ld\n", childCount, (long)startOffset);
        // Seek on the link!!!
        lk->seekTo( startOffset );
        twComponent &cp = lk->getRootComponent();
//        qWarning( "twTrackMix::calcOutputTo(): On Object $%08x. with offset %d.\n",
//                  &cp, playOffset_ );
//        cp.seekTo( startOffset );
        offset_t doRead = endTime-startTime;
        fprintf(stderr, "[Mixer] Child %d: doRead=%ld\n", childCount, (long)doRead);
        // Only zero out the range we'll actually use (not entire playLen)
        offset_t destOffset = startTime-startInterval;
        memset( readBuffer + destOffset, 0, sizeof( sample_t ) * doRead );
        // Get actual amount produced (may be less than doRead if component underruns)
        length_t actuallyGot = cp.calcOutputTo( readBuffer+destOffset, doRead, outChannel );
        fprintf(stderr, "[Mixer] Child %d: actuallyGot=%ld samples\n", childCount, (long)actuallyGot);
        // Only mix the actual samples produced (don't mix zero-padded tail)
        for( offset_t i = 0; i < actuallyGot; i++ ) {
            buffer[destOffset + i] += readBuffer[destOffset + i];
        }
    }
    fprintf(stderr, "[twTrackMix::calcOutputTo] processed %d children\n", childCount);

    // Intrinsic track processing: apply the track's own gain (and mute) here so
    // the output is self-contained — correct wherever it is summed, both by the
    // master mixer and, for groups, by a parent track. Volume is in dB (same
    // convention as twMixer::setInputLevel); read live so changes apply at once.
    double factor = track_.isMuted() ? 0.0 : pow( 10., track_.getVolume()/20. );
    if( factor != 1.0 ) {
        for( offset_t i=0; i<(offset_t)playLen; i++ ) {
            buffer[i] *= (sample_t) factor;
        }
    }
    return playLen;
}



twTrackMix::~twTrackMix()
{
}

twTrackMix::twTrackMix( tw303aEnvironment &env, STrack &track )
    : twComponent( env ),
      track_( track ),
      playOffset_( 0 )
{
}
