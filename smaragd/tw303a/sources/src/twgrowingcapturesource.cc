#include "tw/sources/twgrowingcapturesource.h"

#include "tw/sources/twcapturingsource.h"
#include "tw/pages/tw_output_page.h"
#include "tw/pages/tw_page_accounting.h"

#include <algorithm>
#include <cstring>
#include <new>

// One chunk is one page's worth of ONE channel — the claim the header makes.
static_assert( twGrowingCaptureSource::kDefaultChunkFrames ==
                   twOutputPage::FRAME_CAPACITY,
               "the default chunk is meant to be one page per channel" );

twGrowingCaptureSource::twGrowingCaptureSource( idx_t channels, int sampleRate,
                                                std::size_t chunkFrames,
                                                std::size_t maxChunks )
    : channels_( channels < 1 ? 1 : channels ),
      sampleRate_( sampleRate ),
      chunkFrames_( chunkFrames < 1 ? 1 : chunkFrames ),
      chunks_( maxChunks < 1 ? 1 : maxChunks )
{
    for( auto &c : chunks_ ) c.store( nullptr, std::memory_order_relaxed );
}

twGrowingCaptureSource::~twGrowingCaptureSource()
{
    const std::size_t perChunk = (std::size_t) channels_ * chunkFrames_;
    for( auto &c : chunks_ ) {
        float *p = c.load( std::memory_order_relaxed );
        if( p ) {
            tw::pages::PageAccounting::onCaptureReleased( perChunk * sizeof( float ) );
            delete[] p;
        }
    }
}

std::size_t twGrowingCaptureSource::residentBytes() const
{
    return chunksAllocated() * (std::size_t) channels_ * chunkFrames_ * sizeof( float );
}

std::uint32_t twGrowingCaptureSource::maskedChannels( std::uint32_t channelMask ) const
{
    if( channelMask == 0 ) return (std::uint32_t) channels_;
    std::uint32_t n = 0;
    for( idx_t c = 0; c < channels_; ++c )
        if( channelMask & (1u << (unsigned) c) ) ++n;
    return n ? n : 1u;
}

float *twGrowingCaptureSource::chunkForWrite_( std::uint64_t frame )
{
    const std::size_t idx = (std::size_t)( frame / chunkFrames_ );
    if( idx >= chunks_.size() ) return nullptr;

    float *p = chunks_[ idx ].load( std::memory_order_relaxed );
    if( p ) return p;

    const std::size_t n = (std::size_t) channels_ * chunkFrames_;
    p = new (std::nothrow) float[ n ];
    if( !p ) return nullptr;
    std::memset( p, 0, n * sizeof( float ) );

    // Release: the zero-fill (and, for a later append into this same chunk,
    // the samples) must be visible to a reader that acquires the frontier.
    chunks_[ idx ].store( p, std::memory_order_release );
    chunksAllocated_.fetch_add( 1, std::memory_order_relaxed );
    tw::pages::PageAccounting::onCaptureAllocated( n * sizeof( float ) );
    return p;
}

const float *twGrowingCaptureSource::chunkForRead_( std::uint64_t frame ) const
{
    const std::size_t idx = (std::size_t)( frame / chunkFrames_ );
    if( idx >= chunks_.size() ) return nullptr;
    return chunks_[ idx ].load( std::memory_order_acquire );
}

std::size_t twGrowingCaptureSource::reserveThrough( std::size_t frames )
{
    if( frames == 0 ) return 0;
    std::size_t done = 0;
    for( std::uint64_t f = 0; f < (std::uint64_t) frames; f += chunkFrames_ ) {
        if( !chunkForWrite_( f ) ) break;
        done = (std::size_t) std::min<std::uint64_t>( (std::uint64_t) frames,
                                                      f + chunkFrames_ );
    }
    return done;
}

std::size_t twGrowingCaptureSource::append( const float *interleaved,
                                            std::size_t frames )
{
    if( !interleaved || frames == 0 ) return 0;

    std::uint64_t pos = frontier_.load( std::memory_order_relaxed );
    std::size_t   done = 0;

    while( done < frames ) {
        float *chunk = chunkForWrite_( pos );
        if( !chunk ) break;

        const std::size_t within = (std::size_t)( pos % chunkFrames_ );
        const std::size_t n = std::min( frames - done, chunkFrames_ - within );

        for( idx_t c = 0; c < channels_; ++c ) {
            float       *dst = chunk + (std::size_t) c * chunkFrames_ + within;
            const float *src = interleaved + done * (std::size_t) channels_ + (std::size_t) c;
            for( std::size_t i = 0; i < n; ++i ) {
                dst[ i ] = *src;
                src += channels_;
            }
        }

        pos  += n;
        done += n;
    }

    if( done < frames )
        dropped_.fetch_add( frames - done, std::memory_order_relaxed );

    // The one publication. Everything above happens-before any reader that
    // acquires this value.
    frontier_.store( pos, std::memory_order_release );
    return done;
}

std::size_t twGrowingCaptureSource::appendPlanar( const float *const *planar,
                                                  std::size_t frames )
{
    if( !planar || frames == 0 ) return 0;

    std::uint64_t pos = frontier_.load( std::memory_order_relaxed );
    std::size_t   done = 0;

    while( done < frames ) {
        float *chunk = chunkForWrite_( pos );
        if( !chunk ) break;

        const std::size_t within = (std::size_t)( pos % chunkFrames_ );
        const std::size_t n = std::min( frames - done, chunkFrames_ - within );

        for( idx_t c = 0; c < channels_; ++c ) {
            float *dst = chunk + (std::size_t) c * chunkFrames_ + within;
            if( planar[ c ] )
                std::memcpy( dst, planar[ c ] + done, n * sizeof( float ) );
            else
                std::memset( dst, 0, n * sizeof( float ) );
        }

        pos  += n;
        done += n;
    }

    if( done < frames )
        dropped_.fetch_add( frames - done, std::memory_order_relaxed );

    frontier_.store( pos, std::memory_order_release );
    return done;
}

length_t twGrowingCaptureSource::read( offset_t srcOffset, sample_t *dest,
                                       length_t len, idx_t channel ) const
{
    if( !dest || len <= 0 ) return 0;
    std::memset( dest, 0, (std::size_t) len * sizeof( sample_t ) );
    if( srcOffset < 0 ) return 0;

    // Clamp exactly like twRandomSource promises: mono material is served on
    // every requested channel.
    if( channel < 0 ) channel = 0;
    if( channel >= channels_ ) channel = channels_ - 1;

    const std::uint64_t fr = frontier();
    const std::uint64_t start = (std::uint64_t) srcOffset;
    if( start >= fr ) return 0;

    const std::size_t want =
        (std::size_t) std::min<std::uint64_t>( (std::uint64_t) len, fr - start );

    std::size_t   done = 0;
    std::uint64_t pos  = start;
    while( done < want ) {
        const float *chunk = chunkForRead_( pos );
        if( !chunk ) break;                       // cannot happen below the frontier
        const std::size_t within = (std::size_t)( pos % chunkFrames_ );
        const std::size_t n = std::min( want - done, chunkFrames_ - within );
        std::memcpy( dest + done,
                     chunk + (std::size_t) channel * chunkFrames_ + within,
                     n * sizeof( float ) );
        pos  += n;
        done += n;
    }
    return (length_t) done;
}

std::size_t twGrowingCaptureSource::readInterleaved( std::uint64_t startFrame,
                                                     float *dest,
                                                     std::size_t frames,
                                                     std::uint32_t channelMask ) const
{
    if( !dest || frames == 0 ) return 0;

    const std::uint32_t outCh = maskedChannels( channelMask );
    std::memset( dest, 0, frames * outCh * sizeof( float ) );

    const std::uint64_t fr = frontier();
    if( startFrame >= fr ) return 0;
    const std::size_t want =
        (std::size_t) std::min<std::uint64_t>( (std::uint64_t) frames, fr - startFrame );

    std::size_t   done = 0;
    std::uint64_t pos  = startFrame;
    while( done < want ) {
        const float *chunk = chunkForRead_( pos );
        if( !chunk ) break;
        const std::size_t within = (std::size_t)( pos % chunkFrames_ );
        const std::size_t n = std::min( want - done, chunkFrames_ - within );

        std::uint32_t outIdx = 0;
        for( idx_t c = 0; c < channels_; ++c ) {
            if( channelMask != 0 && !( channelMask & (1u << (unsigned) c) ) )
                continue;
            const float *src = chunk + (std::size_t) c * chunkFrames_ + within;
            float *dst = dest + done * (std::size_t) outCh + outIdx;
            for( std::size_t i = 0; i < n; ++i ) {
                *dst = src[ i ];
                dst += outCh;
            }
            ++outIdx;
        }

        pos  += n;
        done += n;
    }
    return done;
}

std::shared_ptr<twCapturingSource> twGrowingCaptureSource::toCapturingSource(
        std::uint64_t startFrame, length_t frames ) const
{
    const std::uint64_t fr = frontier();
    const std::uint64_t avail = ( startFrame < fr ) ? ( fr - startFrame ) : 0;
    const std::uint64_t n = ( frames < 0 )
                                ? avail
                                : std::min<std::uint64_t>( (std::uint64_t) frames, avail );

    // ONE copy: the flat planar buffer twCapturingSource adopts is filled
    // straight out of the chunks and moved in — never built into a scratch and
    // copied again.
    std::vector<sample_t> flat( (std::size_t) channels_ * (std::size_t) n, 0.0f );
    for( idx_t c = 0; c < channels_; ++c )
        read( (offset_t) startFrame,
              flat.data() + (std::size_t) c * (std::size_t) n,
              (length_t) n, c );

    return std::make_shared<twCapturingSource>( std::move( flat ), (length_t) n,
                                                channels_, sampleRate_ );
}
