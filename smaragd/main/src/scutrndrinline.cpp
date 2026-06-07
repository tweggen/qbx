
#include <stdio.h>
#include <qpainter.h>
#include <qobject.h>

#include "sapplication.h"
#include "slink.h"
#include "scut.h"
#include "scutrndrinline.h"

namespace {
// A render context that maps one loop-repetition's pixel span linearly onto the
// content segment [startOffset, startOffset+loopLength]. The wave renderer
// subtracts lk.getStartTime() from getTimeOf(), so baseTime carries
// (clipStart + startOffset) and the per-pixel ramp covers exactly one segment.
class LoopSegmentContext : public SRenderContext {
public:
    LoopSegmentContext( QPainter &p, offset_t baseTime,
                        double segLeftX, double segWpx, length_t segLen )
        : SRenderContext( p ), baseTime_( baseTime ),
          segLeftX_( segLeftX ), segWpx_( segWpx ), segLen_( segLen ) {}
    virtual offset_t getTimeOf( int x ) const {
        double rel = ( (double) x - segLeftX_ ) * (double) segLen_ / segWpx_;
        if( rel < 0 ) rel = 0;
        return baseTime_ + (offset_t) rel;
    }
private:
    offset_t baseTime_;
    double   segLeftX_;
    double   segWpx_;
    length_t segLen_;
};
}

/**
 * The actual cut renderer function.
 * Non-looping cuts draw their content once. A looping cut tiles its loop segment
 * across the clip width (the wave renderer fetches one linear range per call, so
 * tiling needs repeated draws rather than a wrapped getTimeOf), with a faint
 * divider at each loop boundary.
 */
void SCutRendererInline::draw( SLink &lk, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    QRect visibRect = ctx.getVisibRect();

    SObjectRenderer *rndr = getCut().getContent().getInlineRenderer();
    if( !rndr ) {
        p.drawText( visibRect, Qt::AlignCenter, "SCut: No renderer." );
        return;
    }

    SCut &cut = getCut();
    if( !cut.isLooping() ) {
        // Note, that this is my link but his object!!!
        InlineRenderContext myctx( cut, ctx, p, lk.getStartTime() );
        myctx.setVisibRect( visibRect );
        rndr->draw( lk, myctx );
        return;
    }

    length_t segLen   = cut.getLoopLength();
    offset_t baseTime = lk.getStartTime() + cut.getStartOffset();

    // Pixels-per-sample from two probe points of the parent (timeline) mapping.
    int xa = visibRect.x();
    int xb = visibRect.x() + visibRect.width();
    if( xb <= xa ) xb = xa + 1;
    double ta = (double) ctx.getTimeOf( xa );
    double tb = (double) ctx.getTimeOf( xb );
    double spp = ( tb - ta ) / (double)( xb - xa );     // timeline samples / pixel
    if( spp <= 0.0 ) spp = 1.0;
    double clipLeftX = (double) xa + ( (double) lk.getStartTime() - ta ) / spp;
    double segWpx = (double) segLen / spp;
    if( segWpx < 1.0 ) segWpx = 1.0;

    int right = visibRect.x() + visibRect.width();
    int k = 0;
    if( clipLeftX < visibRect.x() )
        k = (int)( ( visibRect.x() - clipLeftX ) / segWpx );
    for( ; ; k++ ) {
        double sx = clipLeftX + (double) k * segWpx;
        if( sx >= right ) break;
        double ex = sx + segWpx;
        int isx = (int)( sx > visibRect.x() ? sx : visibRect.x() );
        int iex = (int)( ex < right ? ex : right );
        if( iex <= isx ) continue;
        LoopSegmentContext lctx( p, baseTime, sx, segWpx, segLen );
        lctx.setVisibRect( QRect( isx, visibRect.y(), iex - isx, visibRect.height() ) );
        rndr->draw( lk, lctx );
        if( ex < right ) {                              // loop boundary divider
            p.setPen( QColor( 70, 70, 70 ) );
            p.drawLine( (int) ex, visibRect.y(), (int) ex, visibRect.y() + visibRect.height() );
        }
    }
}

/**
 * Return the absolute time (in samples, for now) of the given x position.
 * We calculate it by adjusting the time given by the parent context 
 * with the cut's start offset.
 */
offset_t SCutRendererInline::InlineRenderContext::getTimeOf( int x ) const
{
    // The content (raw source) is indexed in the SOURCE domain, but startOffset
    // and the cut window live in the grain OUTPUT (stretched) domain. Map the
    // clip-relative output position back to the source by dividing by the stretch
    // factor, so the drawn waveform lines up with what plays. (The wave renderer
    // subtracts the link start time again, so we add it back here.)
    double stretch = cut_.getStretch();
    if( stretch <= 0.0 ) stretch = 1.0;
    double rel = (double) parentRC_.getTimeOf( x )
               - (double) clipStart_ + (double) cut_.getStartOffset();
    if( rel < 0 ) rel = 0;
    return clipStart_ + (offset_t)( rel / stretch );
}

SCutRendererInline::InlineRenderContext::~InlineRenderContext()
{
}

SCutRendererInline::InlineRenderContext::InlineRenderContext(
    SCut &cut, SRenderContext &par, QPainter &painter, offset_t clipStart )
    : SRenderContext( painter ),
      parentRC_( par ),
      cut_( cut ),
      clipStart_( clipStart )
{
}


SCutRendererInline::SCutRendererInline( SCut &cut )
    : SObjectRenderer( (SObject &)cut )
{
}

SCutRendererInline::~SCutRendererInline()
{
}
