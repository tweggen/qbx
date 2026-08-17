
#include <string.h>

#include "tw/sources/twcapturingsource.h"
// tw/graph/twcomponent.h and tw/graph/tw303aenv.h were included ONLY for the
// live-component capture constructor, which had zero callers and was deleted at
// proposal 36 B9 (§7 trap 27). A capturing source no longer references the graph
// at all: it adopts a buffer somebody else rendered.
#include "tw/pages/tw_page_accounting.h"

twCapturingSource::twCapturingSource( std::vector<sample_t> &&data, length_t nFrames,
                                      idx_t channels, int sampleRate )
    : sampleRate_( sampleRate ),
      channels_( channels < 0 ? 0 : channels ),
      nFrames_( nFrames < 0 ? 0 : nFrames ),
      data_( std::move( data ) )
{
    // Zero-pad (or size) the buffer to exactly channels_*nFrames_ so read()'s
    // bounds arithmetic holds regardless of what the caller handed us.
    data_.resize( (size_t) channels_ * (size_t) nFrames_, 0.0f );
    accountBuffer_();
}

twCapturingSource::~twCapturingSource()
{
    tw::pages::PageAccounting::onCaptureReleased( accountedBytes_ );
}

void twCapturingSource::accountBuffer_()
{
    accountedBytes_ = data_.size() * sizeof( sample_t );
    tw::pages::PageAccounting::onCaptureAllocated( accountedBytes_ );
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

