
#include <string.h>

#include "twcapturingsource.h"
#include "twcomponent.h"
#include "tw303aenv.h"

twCapturingSource::twCapturingSource( tw303aEnvironment &env, twComponent &source,
                                      offset_t captureStart, length_t nFrames,
                                      idx_t channels, int sampleRate )
    : sampleRate_( sampleRate ),
      channels_( channels ),
      nFrames_( nFrames )
{
    if( nFrames_ <= 0 || channels_ <= 0 ) {
        nFrames_ = ( nFrames_ < 0 ) ? 0 : nFrames_;
        return;                       // nothing to build; read() will zero-fill
    }

    data_.assign( (size_t) channels_ * nFrames_, 0.0f );

    length_t block = env.getBufferSize();
    if( block <= 0 ) block = 4096;
    std::vector<sample_t> tmp( (size_t) block );

    const bool seekable = source.isSeekable();
    for( idx_t c = 0; c < channels_; ++c ) {
        // Rewind to the window start for each channel: twTrackMix (and kin)
        // advance one shared cursor per calcOutputTo call regardless of channel,
        // so each channel needs its own pass from captureStart. A non-seekable
        // source can only be captured in a single forward pass — channel 0.
        if( seekable ) {
            source.seekTo( captureStart );
            // Reset all latches attached to the source so they start from position 0,
            // ensuring deterministic capture output (fixes nil-operation bug where
            // rebuilding without actual changes would produce different audio).
            source.resetAllLatches();
        } else if( c > 0 ) {
            break;
        }
        sample_t *out = data_.data() + (size_t) c * nFrames_;
        length_t done = 0;
        while( done < nFrames_ ) {
            length_t want = nFrames_ - done;
            if( want > block ) want = block;
            // calcOutputTo is expected to fill `want` frames (zero-padding past
            // end-of-content); pre-clear so a short producer still yields silence.
            memset( tmp.data(), 0, sizeof( sample_t ) * want );
            source.calcOutputTo( tmp.data(), want, c );
            memcpy( out + done, tmp.data(), sizeof( sample_t ) * want );
            done += want;
        }
    }
}

twCapturingSource::~twCapturingSource()
{
}

length_t twCapturingSource::read( offset_t srcOffset, sample_t *dest,
                                  length_t len, idx_t channel ) const
{
    if( len <= 0 ) return 0;
    if( nFrames_ <= 0 || channels_ <= 0 ) {
        memset( dest, 0, sizeof( sample_t ) * len );
        return 0;
    }

    idx_t ch = channel;
    if( ch < 0 ) ch = 0;
    if( ch >= channels_ ) ch = channels_ - 1;

    length_t avail = 0;
    if( srcOffset < (offset_t) nFrames_ ) {
        avail = nFrames_ - (length_t) srcOffset;
    }
    length_t n = len;
    if( n > avail ) n = avail;
    if( n < 0 ) n = 0;

    if( n > 0 ) {
        const sample_t *src = data_.data() + (size_t) ch * nFrames_ + srcOffset;
        memcpy( dest, src, sizeof( sample_t ) * n );
    }
    if( n < len ) {
        memset( dest + n, 0, sizeof( sample_t ) * ( len - n ) );
    }
    return n;
}
