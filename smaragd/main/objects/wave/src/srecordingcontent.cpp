#include "app/objects/wave/srecordingcontent.h"
#include "app/objects/wave/srecordingrndrinline.h"

#include "tw/sources/twgrowingcapturesource.h"

#include <algorithm>

SRecordingContent::SRecordingContent( SProject *project,
                                      std::shared_ptr<twGrowingCaptureSource> src,
                                      std::uint64_t startFrame )
    : SObject( project ), src_( std::move( src ) ), startFrame_( startFrame )
{
    setSName( QStringLiteral( "Recording" ) );
}

SRecordingContent::~SRecordingContent()
{
    delete inlineRenderer_;
}

QWidget *SRecordingContent::getDetailEditWidget( QWidget * )
{
    // Nothing to edit: the object exists for the duration of a capture and is
    // replaced by a WAV-backed cut the instant it ends.
    return NULL;
}

QWidget *SRecordingContent::getInlineEditWidget( QWidget * )
{
    return NULL;
}

SObjectRenderer *SRecordingContent::getInlineRenderer()
{
    if( !inlineRenderer_ )
        inlineRenderer_ = new SRecordingRendererInline( *this );
    return inlineRenderer_;
}

twRandomSource *SRecordingContent::getRandomSource()
{
    return src_.get();
}

length_t SRecordingContent::availableFrames() const
{
    if( !src_ ) return 0;
    const std::uint64_t fr = src_->frontier();
    return ( fr > startFrame_ ) ? (length_t)( fr - startFrame_ ) : (length_t) 0;
}

length_t SRecordingContent::getDuration() const
{
    // The PUBLISHED length, not the live frontier. Every consumer that asks
    // (the cut's window, the track's geometry, the renderer's time map) must
    // agree with the last durationChanged the recorder emitted; answering the
    // frontier here would make the drawn clip and its own waveform disagree
    // by up to a tick.
    return published_;
}

length_t SRecordingContent::publishGrowth()
{
    if( frozen_ ) return published_;

    const length_t now = availableFrames();
    if( now <= published_ ) return published_;

    published_ = now;
    extendPeaks_();
    emit durationChanged( published_ );
    return published_;
}

void SRecordingContent::extendPeaks_()
{
    if( !src_ ) return;
    // Only ever forward, and only ever below the frontier that produced
    // `published_`: a short read past it would zero-fill and bake silence into
    // the ladder for frames that are about to arrive.
    const std::uint64_t through = (std::uint64_t) published_;
    if( through <= peaksThrough_ ) return;

    const idx_t nCh = src_->channels();
    if( nCh <= 0 ) { peaksThrough_ = through; return; }
    if( (offset_t) scanBuf_.size() < peakHop_ )
        scanBuf_.assign( (std::size_t) peakHop_, 0.f );

    // Whole hops only. The tail (< one hop) is left for the next call, so a
    // bucket is folded exactly once and never from partial material.
    const std::uint64_t firstHop = peaksThrough_ / (std::uint64_t) peakHop_;
    const std::uint64_t lastHop  = through / (std::uint64_t) peakHop_;
    if( lastHop <= firstHop ) return;

    if( peaks_.size() < (std::size_t) lastHop )
        peaks_.resize( (std::size_t) lastHop );

    for( std::uint64_t h = firstHop; h < lastHop; ++h ) {
        const std::uint64_t at = startFrame_ + h * (std::uint64_t) peakHop_;
        sample_t mn = SAMPLE_NORM_MAX, mx = SAMPLE_NORM_MIN;
        // ALL CHANNELS folded into one signed envelope (proposal 36 B8), the
        // same rule SCut::ensureCapturePeaks and straightCalcPreviewData use.
        for( idx_t c = 0; c < nCh; ++c ) {
            src_->read( (offset_t) at, scanBuf_.data(), peakHop_, c );
            for( offset_t j = 0; j < peakHop_; ++j ) {
                const sample_t a = scanBuf_[(std::size_t) j];
                if( a < mn ) mn = a;
                if( a > mx ) mx = a;
            }
        }
        double mxs = ( mx * 127. ) / SAMPLE_NORM_MAX;
        if( mxs > 127. ) mxs = 127.;
        if( mxs < -128. ) mxs = -128.;
        double mns = ( mn * -127. ) / SAMPLE_NORM_MIN;
        if( mns > 127. ) mns = 127.;
        if( mns < -128. ) mns = -128.;
        peaks_[(std::size_t) h].min = (char) mns;
        peaks_[(std::size_t) h].max = (char) mxs;
    }
    peaksThrough_ = lastHop * (std::uint64_t) peakHop_;
}

int SRecordingContent::getPreview( preview_t *dest, offset_t start,
                                   length_t length, offset_t nProbes )
{
    if( !dest || nProbes <= 0 ) return -1;
    if( length < 1 ) length = 1;
    if( peaks_.empty() ) {
        // Nothing captured yet: a flat midline rather than an error, so an
        // armed track shows an empty clip growing rather than "No Preview"
        // flickering into a waveform.
        for( offset_t k = 0; k < nProbes; ++k ) { dest[k].min = 0; dest[k].max = 0; }
        return 0;
    }

    for( offset_t k = 0; k < nProbes; ++k ) {
        const offset_t s = start + ( k * length ) / nProbes;
        const offset_t e = start + ( ( k + 1 ) * length ) / nProbes;
        offset_t i0 = ( s > 0 ? s : 0 ) / peakHop_;
        offset_t i1 = ( ( e > s ? e - 1 : s ) > 0 ? ( e > s ? e - 1 : s ) : 0 ) / peakHop_;
        char aggMin = 0, aggMax = 0;
        bool any = false;
        for( offset_t i = i0; i <= i1 && i < (offset_t) peaks_.size(); ++i ) {
            const char pmn = peaks_[(std::size_t) i].min;
            const char pmx = peaks_[(std::size_t) i].max;
            if( !any ) { aggMin = pmn; aggMax = pmx; any = true; }
            else {
                if( pmn < aggMin ) aggMin = pmn;
                if( pmx > aggMax ) aggMax = pmx;
            }
        }
        dest[k].min = aggMin;
        dest[k].max = aggMax;
    }
    return 0;
}
