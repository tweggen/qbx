
#include <stdio.h>
#include <qpainter.h>
#include <qobject.h>
#include <QFontMetrics>
#include <QGuiApplication>

#include "app/model/slink.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/scutrndrinline.h"
#include "app/objects/wave/swaveformdraw.h"

namespace {
// A render context that maps one loop-repetition's pixel span onto the content's
// raw SOURCE samples. The loop segment is loopLength samples of grain OUTPUT
// starting at startOffset (both in the stretched OUTPUT domain, like the rest of
// the cut window), but the content waveform is indexed in the SOURCE domain — so
// the output position (startOffset + per-pixel ramp) is divided by the stretch
// factor, exactly as the non-loop InlineRenderContext::getTimeOf() does. Without
// this a stretched, looped clip reads the wrong source range (and on a clip
// slipped far into the sample, past the content end -> a flat zero waveform). The
// wave renderer subtracts lk.getStartTime() from getTimeOf(), so clipStart is
// added back here.
class LoopSegmentContext : public SRenderContext {
public:
    LoopSegmentContext( QPainter &p, offset_t clipStart,
                        const twAffineMap &clipToSource,
                        double segLeftX, double segWpx, length_t segLen )
        : SRenderContext( p ), clipStart_( clipStart ),
          clipToSource_( clipToSource ),
          segLeftX_( segLeftX ), segWpx_( segWpx ), segLen_( segLen ) {}
    virtual offset_t getTimeOf( int x ) const {
        double rel = ( (double) x - segLeftX_ ) * (double) segLen_ / segWpx_;
        if( rel < 0 ) rel = 0;
        // rel is the position inside this repetition; the SHARED
        // clip->source map (the one playback derives its window from,
        // proposal 18 Phase 4) yields the source sample the waveform is
        // read from. Pixels quantize rel; the map itself is exact.
        return clipStart_ + (offset_t) clipToSource_
                   .map( Fraction( (int64_t) rel ) ).floorToInt();
    }
private:
    offset_t    clipStart_;
    twAffineMap clipToSource_;
    double      segLeftX_;
    double      segWpx_;
    length_t    segLen_;
};

// Below this many pixels per repetition the handles would collide with each
// other, so they are neither drawn nor grabbable. The arranger asks the same
// question before hit-testing (SMVActualView::loopMarkerAt).
const int LOOP_HANDLE_MIN_SEG_PX = 2*SCUT_LOOP_HANDLE_W;
}

QRect scutLoopHandleRect( const QRect &clipRect, int x )
{
    // "One character high", in the same 7pt font the small per-clip numbers end
    // up being drawn in. Taken from the application font, NOT from a painter, so
    // the drawn box and the arranger's hit box are always the same size.
    QFont f = QGuiApplication::font();
    f.setPointSize( 7 );
    int h = QFontMetrics( f ).height();
    int maxH = clipRect.height() - 2;
    if( h > maxH ) h = maxH;
    if( h < 4 ) return QRect();                  // lane too short for a grip
    return QRect( x - SCUT_LOOP_HANDLE_W/2, clipRect.y() + 1,
                  SCUT_LOOP_HANDLE_W, h );
}

namespace {
// Paint one loop-marker grab handle: a light box with two grip bars, hinting
// that it drags horizontally.
void drawLoopHandle( QPainter &p, const QRect &clipRect, int x )
{
    QRect box = scutLoopHandleRect( clipRect, x );
    if( box.isNull() ) return;
    p.fillRect( box, QColor( 225, 225, 205 ) );
    p.setPen( QColor( 40, 40, 40 ) );
    p.drawRect( box );
    p.setPen( QColor( 90, 90, 90 ) );
    for( int gx = box.center().x()-1; gx <= box.center().x()+1; gx += 2 )
        p.drawLine( gx, box.y()+2, gx, box.bottom()-2 );
}
}

namespace {
// A transposed clip looks exactly like an untransposed one (pitch never moves
// an edge), so it needs a written mark. Whole semitones read as "+2 st"; a
// fine-nudged clip shows its cents.
void drawPitchBadge( QPainter &p, const QRect &visibRect, double cents )
{
    if( cents == 0.0 ) return;
    const double semis = cents / 100.0;
    const bool   whole = ( cents == (double)(int) cents ) && ( (int) cents % 100 == 0 );
    QString text = whole
        ? QString( "%1%2 st" ).arg( semis > 0 ? "+" : "" ).arg( (int) semis )
        : QString( "%1%2 ct" ).arg( cents > 0 ? "+" : "" ).arg( cents, 0, 'g', 4 );

    QFontMetrics fm( p.font() );
    QRect box = fm.boundingRect( text ).adjusted( -3, -1, 3, 1 );
    box.moveTopLeft( visibRect.topLeft() + QPoint( 2, 2 ) );
    if( !visibRect.contains( box ) ) return;   // clip too narrow for the badge

    p.save();
    p.setPen( Qt::NoPen );
    p.setBrush( QColor( 20, 20, 30, 180 ) );
    p.drawRect( box );
    p.setPen( QColor( 255, 210, 120 ) );
    p.drawText( box, Qt::AlignCenter, text );
    p.restore();
}
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

    SCut &cut = getCut();
    SObject &content = cut.getContent();

    // Container-backed cut (a live asset over a track/mixer sub-arrangement): the
    // content has no sample waveform renderer of its own, so draw a waveform of
    // its RENDERED output, windowed by this cut. The InlineRenderContext folds
    // the cut's startOffset into the time mapping, so the drawn region matches
    // what plays. (Tier 1 pulls the render via getPreview; Tier 2 reads the
    // capture — see SCut::getPreview.)
    if( !content.getRandomSource() ) {
        InlineRenderContext myctx( cut, ctx, p, lk.getStartTime() );
        myctx.setVisibRect( visibRect );
        // Preview the cut: SCut::getPreview reads the capture (shared with the
        // audio render) in the container frame domain, which myctx supplies
        // (it folds the cut's startOffset into the time mapping).
        if( !drawObjectWaveform( cut, lk, myctx, QColor( 120, 200, 255 ) ) ) {
            p.drawText( visibRect, Qt::AlignCenter, "Asset: (no preview)" );
        }
        if( !cut.getSName().isEmpty() ) {
            p.setPen( QColor( 10, 10, 40 ) );
            p.drawText( visibRect, Qt::AlignBottom | Qt::AlignRight, cut.getSName() );
        }
        drawPitchBadge( p, visibRect, cut.getPitchCents() );
        return;
    }

    // Sample-backed cut: delegate to the content's own waveform renderer.
    SObjectRenderer *rndr = content.getInlineRenderer();
    if( !rndr ) {
        p.drawText( visibRect, Qt::AlignCenter, "SCut: No renderer." );
        return;
    }

    if( !cut.isLooping() ) {
        // Note, that this is my link but his object!!!
        InlineRenderContext myctx( cut, ctx, p, lk.getStartTime() );
        myctx.setVisibRect( visibRect );
        rndr->draw( lk, myctx );
        drawPitchBadge( p, visibRect, cut.getPitchCents() );
        return;
    }

    length_t segLen   = cut.getLoopLength().frames();

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
        LoopSegmentContext lctx( p, lk.getStartTime(), cut.clipToSourceMap(),
                                 sx, segWpx, segLen );
        lctx.setVisibRect( QRect( isx, visibRect.y(), iex - isx, visibRect.height() ) );
        rndr->draw( lk, lctx );
        if( ex < right ) {                              // loop boundary divider
            p.setPen( QColor( 70, 70, 70 ) );
            p.drawLine( (int) ex, visibRect.y(), (int) ex, visibRect.y() + visibRect.height() );
            // Grab handle at the divider's upper end; drag it to re-tile the
            // loop (SMVActualView::loopMarkerAt hit-tests the same box).
            if( segWpx >= LOOP_HANDLE_MIN_SEG_PX )
                drawLoopHandle( p, visibRect, (int) ex );
        }
    }
    drawPitchBadge( p, visibRect, cut.getPitchCents() );
}

/**
 * Return the absolute time (in samples, for now) of the given x position.
 * We calculate it by adjusting the time given by the parent context 
 * with the cut's start offset.
 */
offset_t SCutRendererInline::InlineRenderContext::getTimeOf( int x ) const
{
    // The content (raw source) is indexed in the SOURCE domain. The SHARED
    // clip->source map (srcStart + rel/stretch, exact - the same map the
    // playback window derives from, proposal 18 Phase 4) translates the
    // clip-relative position; there is no second stretch computation to
    // disagree with what plays. (The wave renderer subtracts the link start
    // time again, so we add it back here.)
    int64_t rel = (int64_t) parentRC_.getTimeOf( x ) - (int64_t) clipStart_;
    if( rel < 0 ) rel = 0;
    return clipStart_ + (offset_t) cut_.clipToSourceMap()
               .map( Fraction( rel ) ).floorToInt();
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
