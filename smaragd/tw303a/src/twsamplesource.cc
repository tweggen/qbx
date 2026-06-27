
#include <stdio.h>
#include <string.h>

#include <qfile.h>

#include "twsamplesource.h"
#include "twresampledsource.h"

twSampleSource::twSampleSource( tw303aEnvironment &env, const QString &fileName )
    : env_( env ),
      fileName_( fileName ),
      loaded_( false ),
      channels_( 0 ),
      rate_( 0 ),
      bits_( 0 ),
      nFrames_( 0 ),
      resampledRate_( 0 )
{
    if( loadWav() < 0 ) {
        loaded_ = false;
    }
}

twSampleSource::~twSampleSource()
{
    // resampled_ (unique_ptr<twResampledSource>) destroyed here, where the type
    // is complete.
}

twRandomSource *twSampleSource::viewAtRate( int targetRate ) const
{
    twSampleSource *self = const_cast<twSampleSource *>( this );
    if( !loaded_ || targetRate <= 0 || targetRate == rate_ ) {
        return self;   // native rate already matches (the common case)
    }
    if( !resampled_ || resampledRate_ != targetRate ) {
        resampled_.reset( new twResampledSource( *this, targetRate ) );
        resampledRate_ = targetRate;
    }
    return resampled_.get();
}

/**
 * Stateless random read of one channel into dest, zero-filling past the end of
 * the material. Lock-free: it only touches resident, immutable data.
 */
length_t twSampleSource::read( offset_t srcOffset, sample_t *dest,
                               length_t len, idx_t channel ) const
{
    if( len <= 0 ) return 0;
    if( !loaded_ || channels_ <= 0 ) {
        memset( dest, 0, sizeof( sample_t ) * len );
        return 0;
    }

    idx_t ch = channel;
    if( ch < 0 ) ch = 0;
    if( ch >= channels_ ) ch = channels_ - 1;   // mono plays on every channel

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

/**
 * Parse a (not very tolerant) WAV header and decode the whole data chunk into a
 * resident planar Float32 buffer. Header logic is ported from the old
 * twWavInput::findWaveProperties(); the decode assumes 16-bit signed LE PCM.
 */
int twSampleSource::loadWav()
{
#define SLEN 8192
    unsigned char s[SLEN];

    struct STRU_format {
        unsigned char wFormatTag[2];
        unsigned char wChannels[2];
        unsigned char dwSamplesPerSec[4];
        unsigned char dwAvgBytesPerSecond[4];
        unsigned char wBlockAlign[2];
        unsigned char wBitsPerSample[2];
    } *fmtHdr;

#define EX_SHORT(x) ((x)[0]|((x)[1]<<8))
#define EX_LONG(x) ((x)[0]|((x)[1]<<8)|((x)[2]<<16)|((x)[3]<<24))

    QFile file( fileName_ );
    if( !file.open( QIODevice::ReadOnly ) ) {
        qWarning( "twSampleSource: error opening file \"%s\".\n",
                  (const char *) fileName_.toUtf8().constData() );
        return -1;
    }

    if( !file.seek( 0 ) ) return -1;
    memset( s, 0, SLEN );
    file.read( (char *) s, SLEN );

    if( ::strncmp( (char *) s, "RIFF", 4 ) ) return -2;
    if( ::strncmp( (char *) s + 8, "WAVEfmt ", 8 ) ) return -3;
    fmtHdr = (STRU_format *) ( (int *) ( s + 20 ) );
    channels_ = EX_SHORT( fmtHdr->wChannels );
    if( channels_ <= 0 ) return -5;
    rate_ = EX_LONG( fmtHdr->dwSamplesPerSec );
    bits_ = EX_SHORT( fmtHdr->wBitsPerSample );
    if( bits_ < 8 ) return -6;

    qWarning( "twSampleSource: \"%s\": %d channels, %d Hz, %d bits per sample.\n",
              (const char *) fileName_.toUtf8().constData(),
              channels_, rate_, bits_ );

    s[SLEN - 1] = 0;
    unsigned char *data = NULL;
    for( int i = 0; i < ( SLEN - 4 ); i++ ) {
        if( !strncmp( (const char *) ( s + i ), "data", 4 ) ) {
            data = s + i;
            break;
        }
    }
    if( !data ) return -7;

    long dataLen = EX_LONG( ( data + 4 ) );
    long dataStart = ( data - s ) + 8;
    nFrames_ = ( (length_t) dataLen / channels_ ) / ( bits_ / 8 );

    if( bits_ != 16 ) {
        qWarning( "twSampleSource: only 16-bit PCM is supported (\"%s\" is %d-bit).\n",
                  (const char *) fileName_.toUtf8().constData(), bits_ );
        return -8;
    }
    if( nFrames_ <= 0 ) return -9;

    // Read the raw 16-bit data chunk and deinterleave into planar float.
    if( !file.seek( dataStart ) ) return -10;
    length_t rawBytes = (length_t) nFrames_ * channels_ * 2;
    std::vector<unsigned char> raw( (size_t) rawBytes );
    // QFile::read() is NOT guaranteed to fill a large buffer in a single call,
    // so loop until we have every byte (or hit a real EOF). The previous
    // single-shot read could come up short on big files, silently zero-filling
    // the rest while the header-derived length stayed full — which made preview
    // and audio go flat partway through the clip.
    qint64 total = 0;
    while( total < (qint64) rawBytes ) {
        qint64 got = file.read( (char *) raw.data() + total, (qint64) rawBytes - total );
        if( got <= 0 ) break;   // EOF or error
        total += got;
    }
    length_t gotFrames = ( total / 2 ) / channels_;
    if( gotFrames > nFrames_ ) gotFrames = nFrames_;
    if( gotFrames < nFrames_ ) {
        qWarning( "twSampleSource: short read on \"%s\": %lld of %lld frames; "
                  "clamping to the data actually present.\n",
                  (const char *) fileName_.toUtf8().constData(),
                  (long long) gotFrames, (long long) nFrames_ );
        nFrames_ = gotFrames;   // size buffer + clip to real data, no phantom tail
    }

    data_.assign( (size_t) channels_ * nFrames_, 0.0f );
    for( length_t f = 0; f < nFrames_; ++f ) {
        for( idx_t c = 0; c < channels_; ++c ) {
            const unsigned char *p = raw.data() + ( (size_t) f * channels_ + c ) * 2;
            short v = (short) ( p[0] | ( p[1] << 8 ) );
            data_[ (size_t) c * nFrames_ + f ] = (sample_t) v / 32768.0f;
        }
    }

    file.close();
    loaded_ = true;
    fprintf( stderr, "twSampleSource: loaded %lld frames (%lld bytes) resident.\n",
             (long long) nFrames_, (long long) ( data_.size() * sizeof( sample_t ) ) );
    return 0;
}

