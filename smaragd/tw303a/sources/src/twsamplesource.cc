
#include <stdio.h>
#include <string.h>

#include <qfile.h>
#include <qfileinfo.h>
#include <mutex>
#include <vector>

#include <sndfile.h>

#include "tw/sources/twsamplesource.h"
#include "tw/sources/twresampledsource.h"
#include "tw/core/twlog.h"

twSampleSource::twSampleSource( tw303aEnvironment &env, const QString &fileName )
    : env_( env ),
      fileName_( fileName ),
      loaded_( false ),
      channels_( 0 ),
      rate_( 0 ),
      bits_( 0 ),
      nFrames_( 0 )
{
    // 16-bit PCM WAV keeps the hand-rolled fast path (byte-exact with every
    // prior build, so existing render gates are untouched). A .wav that the fast
    // path rejects (e.g. 24/32-bit) falls back to libsndfile; every other format
    // (MP3/FLAC/AIFF/Ogg/Opus) goes straight to libsndfile.
    const QString ext = QFileInfo( fileName_ ).suffix().toLower();
    if( ext == QLatin1String( "wav" ) ) {
        if( loadWav() < 0 && loadSndfile() < 0 ) {
            loaded_ = false;
        }
    } else {
        if( loadSndfile() < 0 ) {
            loaded_ = false;
        }
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

    // Fast path: native rate or not loaded (safe to read without lock—set once at construction)
    if( !loaded_ || targetRate <= 0 || targetRate == rate_ ) {
        return self;
    }

    // Slow path: resampling needed. Use std::call_once for lock-free lazy initialization.
    // CRITICAL: Lock held only for dictionary access, NOT during constructor call.
    // This prevents blocking the audio thread on expensive resampler creation.

    ResampledEntry *entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(resampledMutex_);
        // Insert or get entry for this rate (under lock, brief operation)
        auto& e = resampledCache_[targetRate];
        entry = &e;
    }  // Lock released before std::call_once

    // Call constructor OUTSIDE the lock via std::call_once.
    // If another thread calls viewAtRate concurrently with the same rate:
    // - Both acquire lock, insert into map (happens-before)
    // - Lock released; both call std::call_once
    // - First thread constructs; second thread waits (via once_flag)
    // - Both get the same object (safe, deterministic)
    std::call_once(entry->flag, [&] {
        entry->obj = std::make_shared<twResampledSource>(*this, targetRate);
    });

    return entry->obj.get();
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

    // A read may begin BEFORE the material (a clip anchored ahead of its data):
    // those frames are silence. Without this the arithmetic below runs off the
    // front — `avail = nFrames_ - srcOffset` GROWS for a negative offset, so
    // nothing clamps and `data_ + ch*nFrames_ + srcOffset` points before the
    // buffer. Emit the leading silence, then read the remainder from frame 0.
    if( srcOffset < 0 ) {
        length_t lead = (length_t)( -srcOffset );
        if( lead > len ) lead = len;
        memset( dest, 0, sizeof( sample_t ) * lead );
        dest += lead;
        len  -= lead;
        srcOffset = 0;
        if( len <= 0 ) return 0;
    }

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
    // Content digest over the decoded PCM exactly as assembled — source rate,
    // all channels, planar — so identical material keys identical sidecars
    // regardless of filename/mtime/project (proposal 27).
    contentHash_ = twHashBuffer( data_.data(), data_.size() * sizeof( sample_t ) );
    loaded_ = true;
    TW_LOGI( "sources", "twSampleSource: loaded %lld frames (%lld bytes) resident, content %s.",
             (long long) nFrames_, (long long) ( data_.size() * sizeof( sample_t ) ),
             contentHash_.toHex().c_str() );
    return 0;
}

// General-purpose importer: decode any libsndfile-readable file (MP3, FLAC,
// AIFF, Ogg/Opus, and non-16-bit WAV) to the same resident planar-Float32 layout
// loadWav() produces, so every downstream reader is format-agnostic. MP3 read
// requires a libsndfile built with mpg123.
int twSampleSource::loadSndfile()
{
    SF_INFO info;
    memset( &info, 0, sizeof( info ) );   // format must be zero before an SFM_READ open

#ifdef _WIN32
    // libsndfile can't open a UTF-8 path on Windows; hand it the wide spelling
    // so non-ASCII sample paths decode.
    const std::wstring wpath = fileName_.toStdWString();
    SNDFILE *snd = sf_wchar_open( wpath.c_str(), SFM_READ, &info );
#else
    const QByteArray path8 = fileName_.toUtf8();
    SNDFILE *snd = sf_open( path8.constData(), SFM_READ, &info );
#endif
    if( !snd ) {
        qWarning( "twSampleSource: libsndfile cannot open \"%s\": %s.\n",
                  (const char *) fileName_.toUtf8().constData(), sf_strerror( nullptr ) );
        return -1;
    }

    if( info.channels <= 0 || info.samplerate <= 0 || info.frames <= 0 ) {
        qWarning( "twSampleSource: libsndfile reports empty/invalid stream for \"%s\".\n",
                  (const char *) fileName_.toUtf8().constData() );
        sf_close( snd );
        return -2;
    }

    channels_ = (idx_t) info.channels;
    rate_     = info.samplerate;
    // Bit depth is informational only (libsndfile hands us float regardless);
    // report 32 for the float pipeline so the log line reads sensibly.
    bits_     = 32;
    nFrames_  = (length_t) info.frames;

    qWarning( "twSampleSource: \"%s\" (libsndfile): %d channels, %d Hz, %lld frames.\n",
              (const char *) fileName_.toUtf8().constData(),
              (int) channels_, rate_, (long long) nFrames_ );

    // Read interleaved float frames, looping until the whole stream is resident
    // (sf_readf_float need not satisfy a large request in one call), then
    // deinterleave into the planar buffer exactly as loadWav() lays it out.
    std::vector<float> inter( (size_t) nFrames_ * channels_ );
    sf_count_t total = 0;
    while( total < info.frames ) {
        sf_count_t got = sf_readf_float( snd,
                                         inter.data() + (size_t) total * channels_,
                                         info.frames - total );
        if( got <= 0 ) break;   // EOF or decode error
        total += got;
    }
    sf_close( snd );

    if( total < info.frames ) {
        qWarning( "twSampleSource: short decode on \"%s\": %lld of %lld frames; "
                  "clamping to the data actually present.\n",
                  (const char *) fileName_.toUtf8().constData(),
                  (long long) total, (long long) info.frames );
        nFrames_ = (length_t) total;   // size + clip to real data, no phantom tail
    }
    if( nFrames_ <= 0 ) return -3;

    data_.assign( (size_t) channels_ * nFrames_, 0.0f );
    for( length_t f = 0; f < nFrames_; ++f ) {
        for( idx_t c = 0; c < channels_; ++c ) {
            data_[ (size_t) c * nFrames_ + f ] =
                inter[ (size_t) f * channels_ + c ];
        }
    }

    // Same digest recipe as loadWav(): over the assembled source-rate planar
    // Float32, so identical decoded material keys identical sidecars regardless
    // of filename/mtime/project or on-disk format (proposal 27).
    contentHash_ = twHashBuffer( data_.data(), data_.size() * sizeof( sample_t ) );
    loaded_ = true;
    TW_LOGI( "sources", "twSampleSource: loaded %lld frames (%lld bytes) resident via libsndfile, content %s.",
             (long long) nFrames_, (long long) ( data_.size() * sizeof( sample_t ) ),
             contentHash_.toHex().c_str() );
    return 0;
}

