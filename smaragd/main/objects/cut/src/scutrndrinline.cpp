
#include <stdio.h>
#include <cmath>
#include <qpainter.h>
#include <qobject.h>
#include <QFontMetrics>
#include <QGuiApplication>

#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/scutrndrinline.h"
#include "app/objects/wave/splainwave.h"
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
                        const SCut &cut,
                        double segLeftX, double segWpx, length_t segLen )
        : SRenderContext( p ), clipStart_( clipStart ),
          cut_( cut ),
          segLeftX_( segLeftX ), segWpx_( segWpx ), segLen_( segLen ) {}
    virtual offset_t getTimeOf( int x ) const {
        double rel = ( (double) x - segLeftX_ ) * (double) segLen_ / segWpx_;
        if( rel < 0 ) rel = 0;
        // rel is the position inside this repetition; the SHARED
        // clip->source map (the one playback derives its window from,
        // proposal 18 Phase 4) yields the source sample the waveform is
        // read from. Pixels quantize rel; the map itself is exact.
        // W1: map-aware conversion (piecewise when warp anchors exist).
        return clipStart_ + (offset_t) cut_
                   .clipToSource( Fraction( (int64_t) rel ) ).floorToInt();
    }
private:
    offset_t    clipStart_;
    const SCut &cut_;
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
void drawPitchBadge( QPainter &p, const QRect &visibRect, double cents,
                     bool preserveFormants )
{
    // W4 follow-up: the formant flag is part of the badge ("+12 st F") so the
    // armed state is VISIBLE on the clip — a requester listening session
    // reported "no difference", and without an indicator there is no way to
    // tell a disengaged toggle from an ineffective DSP. Preservation without
    // a transposition is inaudible by design, so the badge only shows when a
    // pitch is set.
    if( cents == 0.0 ) return;
    const double semis = cents / 100.0;
    const bool   whole = ( cents == (double)(int) cents ) && ( (int) cents % 100 == 0 );
    QString text = whole
        ? QString( "%1%2 st" ).arg( semis > 0 ? "+" : "" ).arg( (int) semis )
        : QString( "%1%2 ct" ).arg( cents > 0 ? "+" : "" ).arg( cents, 0, 'g', 4 );
    if( preserveFormants )
        text += QStringLiteral( " F" );

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

namespace {
// Salience floor for painting onset ticks. The W0 ground-truth harness
// (analyzers_test) tunes the v3 detector so that AT THIS THRESHOLD the
// strong-attack corpus clears F >= 0.9 while steady tone and the crescendo
// trap fire ZERO UI-tier marks — see proposal 28 §W0. The vocoder's
// keyframes ignore salience (they take every onset); this filter is the
// marker UI's alone.
constexpr float kUiSalience = 0.3f;

// Overlay the salience-filtered onset ticks (amber, top edge) and the user
// warp-marker handles (cyan guide line + downward triangle) on a clip. All
// positions run through the clip's EXACT warp map (piecewise when anchors
// exist, scalar otherwise). Positions map linearly clipDuration -> width; a
// non-positive duration skips everything.
void drawWarpMarkers( QPainter &p, const QRect &visibRect, SRenderContext &ctx,
                      SLink &lk, SCut &cut )
{
    const int64_t clipDuration = (int64_t) cut.getDuration();
    if( clipDuration <= 0 || visibRect.width() <= 0 ) return;
    const int64_t startOff = cut.getStartOffset().frames();

    // VIRTUAL-axis mapping (the requester-reported bug: mapping against the
    // clipped visibRect froze tick spacing once a clip outgrew the viewport).
    // Probe the parent mapping for samples-per-pixel, derive the clip's
    // virtual left edge, and clip-test each glyph against visibRect instead.
    int xa = visibRect.x();
    int xb = visibRect.x() + visibRect.width();
    if( xb <= xa ) xb = xa + 1;
    const double ta = (double) ctx.getTimeOf( xa );
    const double tb = (double) ctx.getTimeOf( xb );
    double spp = ( tb - ta ) / (double)( xb - xa );
    if( spp <= 0.0 ) spp = 1.0;
    const double clipLeftX = (double) xa
                           + ( (double) lk.getStartTime() - ta ) / spp;
    const int left  = visibRect.x();
    const int right = visibRect.x() + visibRect.width() - 1;

    // Loop tiling: a glyph at clip-relative rel repeats at rel + k*segLen,
    // matching the waveform tiles. The DISPLAYED extent of the clip is
    // visibRect (the timeline sizes it from the link; the waveform painter
    // likewise just fills vr) — cut.getDuration() is the CUT's length, which
    // can exceed the displayed window, so pixel bounds, not duration, must
    // terminate the tiling. NB the callback must not be named `emit`: that is
    // a Qt keyword macro expanding to nothing, which silently turns the
    // invocation into a discarded-value statement.
    const bool    looping = cut.isLooping();
    const int64_t segLen  = looping ? cut.getLoopLength().frames()
                                    : clipDuration;
    const double  segWpx  = (double) segLen / spp;

    auto eachTileX = [&]( int64_t rel, auto &&putGlyph ) {
        if( rel < 0 || rel >= segLen ) return;
        double x = clipLeftX + (double) rel / spp;
        if( looping && segWpx > 0.0 && x < (double) left ) {
            // Skip whole repetitions scrolled/clipped off to the left.
            const double k = std::ceil( ( (double) left - x ) / segWpx );
            x += k * segWpx;
        }
        for( ; x <= (double) right; x += segWpx ) {
            if( x >= (double) left ) putGlyph( (int) x );
            if( !looping ) break;
        }
    };

    // --- onset ticks: only for sample-backed SPlainWave content ---
    if( SPlainWave *pw = dynamic_cast<SPlainWave *>( &cut.getContent() ) ) {
        std::shared_ptr<const SPlainWave::UiOnsets> ui = pw->onsetsForUi();
        SProject *proj = cut.getProjectSafe();
        if( ui && !ui->onsets.empty() && ui->sourceRate > 0 && proj ) {
            const double projRate = (double) proj->getSRate();
            const double srcRate  = (double) ui->sourceRate;
            const QColor amber( 255, 200, 60, 200 );
            for( const twOnset &o : ui->onsets ) {
                if( o.salience < kUiSalience ) continue;
                const int64_t projPos = (int64_t) std::llround(
                    (double) o.pos * projRate / srcRate );
                const int64_t warped =
                    cut.sourceToWarpedExact( Fraction( projPos ) ).floorToInt();
                eachTileX( warped - startOff, [&]( int x ) {
                    p.fillRect( x - 1, visibRect.y(), 2, 5, amber );
                } );
            }
        }
    }

    // --- user warp-marker handles (any content kind) ---
    const std::vector<twWarpAnchor> &anchors = cut.getGrainParams().warpAnchors;
    if( !anchors.empty() ) {
        p.save();
        const QColor cyan( 80, 200, 255 );
        for( const twWarpAnchor &a : anchors ) {
            eachTileX( a.warped - startOff, [&]( int x ) {
                p.setPen( QColor( 80, 200, 255, 150 ) );   // translucent guide
                p.drawLine( x, visibRect.y(),
                            x, visibRect.y() + visibRect.height() );
                // 5x7 handle box, black outline, cyan fill (requester spec —
                // the bare triangle was too hard to spot).
                p.setPen( QColor( 0, 0, 0 ) );
                p.setBrush( cyan );
                p.drawRect( x - 2, visibRect.y(), 4, 6 );   // 5x7 incl. pen
            } );
        }
        p.restore();
    }
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

    // Container-backed cut (a live asset over a track/mixer sub-arrangement) has
    // no sample waveform renderer of its own, so it draws a waveform of its
    // RENDERED output (the capture shared with the audio render), windowed by
    // this cut. A sample-backed cut delegates to the content's own renderer.
    // Both fold the cut's startOffset into the time mapping (via the
    // InlineRenderContext / LoopSegmentContext) so the drawn region matches what
    // plays.
    const bool container = !content.getRandomSource();
    SObjectRenderer *rndr = container ? NULL : content.getInlineRenderer();
    if( !container && !rndr ) {
        p.drawText( visibRect, Qt::AlignCenter, "SCut: No renderer." );
        return;
    }

    // Draw one linear repetition of the cut into `segCtx`. A container cut pulls
    // its rendered preview via SCut::getPreview (which reads the capture in the
    // container frame domain that segCtx supplies); a sample cut delegates to the
    // content renderer. Returns false only when a container cut has no preview
    // yet, so the non-loop path can show a placeholder.
    auto drawSeg = [&]( SRenderContext &segCtx ) -> bool {
        if( container )
            return drawObjectWaveform( cut, lk, segCtx, QColor( 120, 200, 255 ) );
        rndr->draw( lk, segCtx );
        return true;
    };

    if( !cut.isLooping() ) {
        // Note, that this is my link but his object!!!
        InlineRenderContext myctx( cut, ctx, p, lk.getStartTime() );
        myctx.setVisibRect( visibRect );
        if( !drawSeg( myctx ) )
            p.drawText( visibRect, Qt::AlignCenter, "Asset: (no preview)" );
    } else {
        // Looping: tile the loop segment across the clip width. The wave renderer
        // / getPreview each fetch one linear range per call, so tiling means
        // repeated draws (rather than a wrapped getTimeOf), with a faint divider
        // and grab handle at each loop boundary. Tiling is what makes the preview
        // CONTINUE past the content end for BOTH cut kinds: a container cut's
        // capture is zero past its natural length, so a single linear pass would
        // otherwise draw a flat/zero tail — matching a wave-backed looped cut.
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
            LoopSegmentContext lctx( p, lk.getStartTime(), cut,
                                     sx, segWpx, segLen );
            lctx.setVisibRect( QRect( isx, visibRect.y(), iex - isx, visibRect.height() ) );
            drawSeg( lctx );
            if( ex < right ) {                              // loop boundary divider
                p.setPen( QColor( 70, 70, 70 ) );
                p.drawLine( (int) ex, visibRect.y(), (int) ex, visibRect.y() + visibRect.height() );
                // Grab handle at the divider's upper end; drag it to re-tile the
                // loop (SMVActualView::loopMarkerAt hit-tests the same box).
                if( segWpx >= LOOP_HANDLE_MIN_SEG_PX )
                    drawLoopHandle( p, visibRect, (int) ex );
            }
        }
    }

    // Onset ticks + user warp-marker handles over the waveform (proposal 28 W2).
    drawWarpMarkers( p, visibRect, ctx, lk, cut );

    // Container cuts show their asset name (bottom-right).
    if( container && !cut.getSName().isEmpty() ) {
        p.setPen( QColor( 10, 10, 40 ) );
        p.drawText( visibRect, Qt::AlignBottom | Qt::AlignRight, cut.getSName() );
    }
    drawPitchBadge( p, visibRect, cut.getPitchCents(),
                    cut.getPreserveFormants() );
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
    // W1: map-aware conversion (piecewise when warp anchors exist).
    return clipStart_ + (offset_t) cut_
               .clipToSource( Fraction( rel ) ).floorToInt();
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
