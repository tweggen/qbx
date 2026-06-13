
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
    fprintf(stderr, "[twTrackMix::seekTo] Setting playOffset_ from %llu to %llu\n",
            (unsigned long long)playOffset_, (unsigned long long)newOffset);
    fflush(stderr);
    playOffset_ = newOffset;
    fprintf(stderr, "[twTrackMix::seekTo] playOffset_ now = %llu\n", (unsigned long long)playOffset_);
    fflush(stderr);
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
    offset_t startInterval = playOffset_;
    playOffset_ += playLen;
    offset_t endInterval = playOffset_;

//      qWarning( "twTrackMix::calcOutputTo(): Called. track_=$%08x; playOffset_ = %d.\n",
//                &track_, playOffset_ );
    memset( buffer, 0, sizeof( sample_t )*playLen );

    // FIXME: Take advantage of sorted objects.
    for( SLink *lk : track_.childLinks() ) {
        if( !lk->hasStartTime() ) continue;
        offset_t startTime = lk->getStartTime();        
        if( startTime>=endInterval ) continue;
        offset_t endTime = startTime;
        if( lk->getSObject().hasDuration() ) {
            endTime += lk->getSObject().getDuration();
            if( startInterval>=endTime ) continue;
        } 
        // This object is affected. Add the parts into the output buffer.
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
        // Seek on the link!!!
        lk->seekTo( startOffset );
        twComponent &cp = lk->getRootComponent();
//        qWarning( "twTrackMix::calcOutputTo(): On Object $%08x. with offset %d.\n",
//                  &cp, playOffset_ );
//        cp.seekTo( startOffset );
        offset_t doRead = endTime-startTime;
        memset( readBuffer, 0, sizeof( sample_t ) * playLen );
        cp.calcOutputTo( readBuffer+startTime-startInterval, doRead, outChannel );
        for( int i=startTime-startInterval; (offset_t)i<endTime-startInterval; i++ ) {
            buffer[i] += readBuffer[i];
        }
    }

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
