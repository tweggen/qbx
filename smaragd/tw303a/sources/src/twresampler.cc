#include "tw/sources/twresampler.h"

#include <cmath>

twResampler::twResampler()
    : inRate_( 0 ),
      outRate_( 0 ),
      phase_( 0.0 )
{
}

void twResampler::configure( std::uint32_t inRate, std::uint32_t outRate )
{
    if( inRate == inRate_ && outRate == outRate_ ) return;
    inRate_  = inRate;
    outRate_ = outRate;
    reset();
}

void twResampler::reserveHint( length_t maxOutFrames )
{
    if( maxOutFrames <= 0 || outRate_ == 0 ) return;
    const double ratio = (double) inRate_ / (double) outRate_;
    // +2 for the interpolation neighbour and the at-most-one carried sample.
    const std::size_t need =
        (std::size_t) std::ceil( ratio * (double) maxOutFrames ) + 2;
    if( hist_.capacity() < need ) hist_.reserve( need );
}


length_t twResampler::process( twLatchStreamingOutput *src,
                               sample_t *out, length_t outFrames,
                               length_t *inConsumed )
{
    if( inConsumed ) *inConsumed = 0;
    if( !src || out == nullptr || outFrames <= 0 ) return 0;

    // Equal rates: read straight through. Output frames == input frames.
    if( isPassthrough() ) {
        length_t got = src->readStreamingData( out, outFrames );
        if( got < 0 ) got = 0;
        if( inConsumed ) *inConsumed = got;
        return got;
    }

    const double ratio = (double) inRate_ / (double) outRate_;

    // Input index range the requested block will touch. The last output sample
    // (k = outFrames-1) reads input floor(pos) and floor(pos)+1, so we need
    // indices [0 .. floor(lastPos)+1] available in hist_.
    const double   lastPos   = phase_ + ratio * (double)( outFrames - 1 );
    const length_t needCount = (length_t) std::floor( lastPos ) + 2;

    // Pull whatever is still missing, in one block, appending to hist_.
    if( (length_t) hist_.size() < needCount ) {
        const length_t   want = needCount - (length_t) hist_.size();
        const std::size_t base = hist_.size();
        hist_.resize( base + (std::size_t) want );
        length_t got = src->readStreamingData( hist_.data() + base, want );
        if( got < 0 ) got = 0;
        if( got < want ) hist_.resize( base + (std::size_t) got );  // source dry
    }

    const length_t avail = (length_t) hist_.size();

    // Interpolate. Stop early if the source could not supply enough input; the
    // backend treats a short return as trailing silence.
    length_t produced = 0;
    for( length_t k = 0; k < outFrames; ++k ) {
        const double   pos = phase_ + ratio * (double) k;
        const length_t i   = (length_t) std::floor( pos );
        if( i + 1 >= avail ) break;
        const double f = pos - (double) i;
        out[k] = (sample_t)( (double) hist_[i] * ( 1.0 - f )
                           + (double) hist_[i + 1] * f );
        ++produced;
    }

    // Advance phase by what we produced and drop fully-consumed input from the
    // front. `consumed` is the input-domain advance the caller wants for the
    // playback locator.
    double   newPhase = phase_ + ratio * (double) produced;
    length_t consumed = (length_t) std::floor( newPhase );
    if( consumed > avail ) consumed = avail;
    if( consumed > 0 ) {
        hist_.erase( hist_.begin(), hist_.begin() + (std::size_t) consumed );
        newPhase -= (double) consumed;
    }
    phase_ = newPhase;

    if( inConsumed ) *inConsumed = consumed;
    return produced;
}

void twResampler::reset()
{
    // Stateless component: position advances in process()
}
