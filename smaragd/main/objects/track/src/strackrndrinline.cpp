#include <stdio.h>
#include <qpainter.h>
#include <qobject.h>
#include <QVarLengthArray>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "app/model/sappcontext.h"
#include "app/model/slink.h"
#include "app/objects/track/strack.h"
#include "app/model/sproject.h"
#include "app/objects/track/sfeelflowbounce.h"   // SFeelFlowUiData
#include "app/objects/track/strackcolormodifier.h"
#include "app/objects/track/strackrndrinline.h"

QColor STrackRendererInline::laneFillColor( STrack &track )
{
    // Check if this track is selected
    bool isTrackSelected = false;
    SProject *project = SAppContext::get().getCurrentProject();
    if( project ) {
        SObject *root = project->getRootComponent();
        if( root ) {
            if( root && root->activeLane() == &track ) {
                isTrackSelected = true;
            }
        }
    }

    // Determine base color (depends on selection state)
    QColor baseColor = isTrackSelected ? QColor( 60, 90, 130 ) : QColor( 40, 70, 100 );

    // Apply track state modifiers (muted, solo, armed for recording)
    STrackColorModifier mod = STrackColorModifier::fromTrackState( track );
    return mod.apply( baseColor );
}

// THE FOLDER-SUM OVERLAY (proposal 39 M3, design D4).
//
// A folder lane used to be a blank rectangle: the clip loop below skips every
// child track (correctly - a child is its own lane), so collapsing a folder made
// the arrangement under it disappear from the screen entirely. This paints the
// summed envelope of everything below it, faintly, on the lane background.
//
// It is drawn HERE and not at the canvas's own seam (SMVActualView::paintEvent),
// because this renderer's fillRect() above would paint straight over it on the
// very next line - and here it also lands in the right order: above the lane
// background, BEHIND the folder's own clips, which the loop below draws after us.
//
// The colour is DERIVED from the lane's own final colour rather than being a
// fourth hardcoded constant, so it follows selection and every
// STrackColorModifier state (muted / solo / armed) for free. Two relations
// matter and are what the gate asserts: strictly LIGHTER than the lane fill (so
// it reads as material), and DARKER than the clip body (QColor(160,160,160)
// below), so it reads as background and never competes with a real clip.
//
// Nothing here blocks: STrack::collectChildSumEnvelope() reads previews that
// already exist (main/timeline/CONTRACT.md inv. 1).
static void drawChildSumOverlay( QPainter &p, const QRect &visibRect,
                                 SRenderContext &ctx, STrack &track,
                                 const QColor &finalColor )
{
    if( !track.hasChildTracks() ) return;

    const SEnvelopeWindow win = envelopeWindowOfContext( ctx );
    const int w = win.width;
    if( w < 1 || visibRect.height() < 2 ) return;

    QVarLengthArray<preview_t> pv( w );
    for( int i = 0; i < w; ++i ) { pv[i].min = 0; pv[i].max = 0; }
    // Returns false when NOTHING contributed - a track with no child tracks, a
    // folder whose children are empty, every child inaudible - and then this
    // draws nothing at all, which is what "at least when they are non-zero"
    // asks for.
    if( !track.collectChildSumEnvelope( win, pv.data() ) ) return;

    QColor c = finalColor.lighter( 140 );
    c.setAlpha( 140 );                      // ~55 %: present, never assertive
    p.setPen( c );

    const int tl     = visibRect.topLeft().x();
    const int top    = visibRect.topLeft().y();
    const int height = visibRect.height();
    for( int i = 0; i < w; ++i ) {
        const int mn = pv[i].min;
        const int mx = pv[i].max;
        if( !mn && !mx ) continue;          // silence draws nothing, not a line
        const int y1 = top + ( ( 127 - mn ) * height ) / 256;
        const int y2 = top + ( ( 127 - mx ) * height ) / 256;
        p.drawLine( i + tl, y1, i + tl, y2 );
    }
}

// --- proposal 40 "Feel Flow" M2: the compliance heatmap band --------------
//
// Per-column colour for the compliance heatmap (design section 4.4, kickoff
// AC 2). ORIGINALLY (2026-08-20) this interpolated between the lane's own
// fill colour and the clip body's fixed grey at partial alpha, chosen so the
// M2 pixel gate's colour relation (strictly lighter than the fill, strictly
// darker than the clip body) held BY CONSTRUCTION. The requester found that
// too subtle to read at a glance ("rainbow, in case I miss shades of grey")
// and asked for AGGRESSIVE, unambiguous colour instead -- so as of the
// 2026-08-21 follow-up the band is painted OPAQUE from
// STrackRendererInline::feelFlowPalette(), a quantized 24-step hue ramp, RED
// (low compliance) through YELLOW to GREEN (high compliance) -- traffic-light
// semantics: red is the hot spot to edit, listen to, or overdub. The relation
// the old comment described no longer applies (there is no lane-fill mixing
// left to reason about); the new gate is exact-RGB LUT membership instead
// (SMainWindow::describeLaneOverlay's band-mode classification), which is a
// STRONGER assertion than a luminance relation -- opaque paint makes an exact
// match sound, with no compositing variation to tolerate.
int STrackRendererInline::feelFlowPaletteIndex( float compliance )
{
    compliance = qBound( 0.0f, compliance, 1.0f );
    int idx = (int) ( compliance * (float) kFeelFlowPaletteSize );
    if( idx >= kFeelFlowPaletteSize ) idx = kFeelFlowPaletteSize - 1;
    return idx;
}

const std::array<QRgb, STrackRendererInline::kFeelFlowPaletteSize> &
STrackRendererInline::feelFlowPalette()
{
    // Function-local static: computed exactly once, on first use (from the UI
    // thread -- the paint path never runs off it), never per-column.
    static const std::array<QRgb, kFeelFlowPaletteSize> palette = [] {
        std::array<QRgb, kFeelFlowPaletteSize> pal{};
        for( int i = 0; i < kFeelFlowPaletteSize; ++i ) {
            // 0 deg (red) .. 120 deg (green) over the 24 steps inclusive.
            const int hue = ( i * 120 ) / ( kFeelFlowPaletteSize - 1 );
            QColor c;
            c.setHsv( hue, 255, 217 );          // value ~0.85 * 255
            pal[i] = c.rgba() | 0xff000000u;     // fully opaque, alpha 255
        }
        return pal;
    }();
    return palette;
}

// Drawn AFTER the clip loop, unlike the folder-sum overlay above, which is
// deliberately BEHIND clips (D4's own rationale there: a folder's own clips
// must stay on top of its child-sum hint). The heatmap targets exactly the
// clip-covered lanes this feature is about, so the "right after
// drawChildSumOverlay" slot the design's v2 text originally named would have
// painted every column invisible under a clip body -- found at kickoff,
// corrected here (section 4.4, AC 2): a bottom band, ~20% of the lane
// height, drawn LAST so it sits visibly on top of everything else.
//
// Visibility rule (AC 3): a missing OR stale track analysis paints NOTHING
// -- two cheap reads (feelFlowStale(), the atomically-cached
// feelFlowForUi()), never a demand, never a block (timeline CONTRACT inv. 1;
// feelFlowForUi()'s own doc covers the one bounded sidecar read its FIRST
// call after a fresh result may do, mirroring onsetsForUi()).
//
// Timeline frame -> hop index is PLACEMENT ARITHMETIC ONLY: the bounce is a
// whole-project render starting at project frame 0
// (sfeelflowbounce.h/.cpp's RenderParams), so its hop grid already sits in
// the SAME frame domain ctx.getTimeOf() returns here -- no warp map, no
// clip-relative mapping, anywhere in this function.
//
// No `finalColor` parameter (unlike drawChildSumOverlay above): the
// 2026-08-21 palette follow-up made the band colour a pure function of
// compliance alone (STrackRendererInline::feelFlowPalette()), independent of
// the lane's own fill -- that is the whole point of an unambiguous
// traffic-light ramp, and it is also what makes the LUT gate an exact-RGB
// match rather than a per-track-colour-dependent relation.
static void drawFeelFlowBand( QPainter &p, const QRect &visibRect,
                              SRenderContext &ctx, STrack &track )
{
    if( visibRect.width() < 1 || visibRect.height() < 2 ) return;
    if( track.feelFlowStale() ) return;
    std::shared_ptr<const SFeelFlowUiData> ui = track.feelFlowForUi();
    if( !ui || ui->hopFrames == 0 || ui->compliance.empty() ) return;

    const int bandHeight = qMax( 2, visibRect.height() / 5 );   // ~20%
    const int bandTop    = visibRect.bottom() - bandHeight + 1;
    const int x0         = visibRect.left();
    const size_t nHops   = ui->compliance.size();
    const auto &palette  = STrackRendererInline::feelFlowPalette();

    for( int i = 0; i < visibRect.width(); ++i ) {
        const offset_t frame = ctx.getTimeOf( x0 + i );
        if( frame < 0 ) continue;               // before the project start
        const uint64_t hop = (uint64_t) frame / (uint64_t) ui->hopFrames;
        if( hop >= nHops ) continue;             // past the analyzed material
        const int idx = STrackRendererInline::feelFlowPaletteIndex(
            ui->compliance[hop] );
        p.setPen( QColor::fromRgba( palette[idx] ) );
        p.drawLine( x0 + i, bandTop, x0 + i, visibRect.bottom() );
    }
}

/**
 * The actual track renderer function.
 * This one should render first the backings, then ask the contents
 * to render themselves into their off-screens, to then transfer them to screen.
 */
void STrackRendererInline::draw( SLink &, SRenderContext &ctx )
{
    QPainter &p = ctx.getPainter();
    QRect visibRect = ctx.getVisibRect();

    offset_t leftTime = ctx.getTimeOf( visibRect.x() );
    offset_t rightTime = ctx.getTimeOf( visibRect.x()+visibRect.width() );

    const QColor finalColor = laneFillColor( getTrack() );

    // Draw background with the final color
    p.fillRect( visibRect, finalColor );    

    // The folder's child-sum overlay goes here: after the fill (which would
    // otherwise erase it) and before the clip loop (so the folder's own clips
    // sit on top of it). Proposal 39 M3.
    drawChildSumOverlay( p, visibRect, ctx, getTrack(), finalColor );

    // BELOW the overlay on purpose. isEmpty() is childOrder_.isEmpty(), and a
    // folder's child TRACKS are child links - so it is already false for the
    // common folder and the move changes no behaviour today. It is here anyway
    // because the reading it invites ("a folder holds no clips of its own, so
    // it is empty") is the one that would silently take the overlay away, and
    // one line of ordering is cheaper than finding that out later.
    if( getTrack().isEmpty() ) {
//        p.setPen( QColor( 160, 64, 64 ) );
//        p.drawText( visibRect, AlignCenter, "Track is empty." );
        return;
    }
    // qWarning( "visibRect.x() = %d, leftTime = %d; rightTime=%d.\n", visibRect.x(), (int)leftTime, (int)rightTime );

    // D11 (proposal 41): PAINT ORDER is deterministic — start-time ascending,
    // latest start on top — rather than childLinks()' insertion order, which
    // is arbitrary and unpredictable to a user. The payoff (M6) is a bottom-
    // left tag that is ALWAYS visible: a clip's left edge can only be covered
    // by a clip painting above it, which by this rule starts later, i.e.
    // strictly to the right, and so can never reach that pixel. The single
    // exception is equal start times, which is exactly why the tiebreak is
    // spelled out here rather than left to childOrder_: child index, then
    // object id (the link's SObject address — the same "objectId" spelling
    // slink.cpp already serializes).
    //
    // The filter pass below is UNCHANGED from before this milestone (same
    // skip-folder-children, hasStartTime/hasDuration, visible-window rules);
    // only the paint ORDER of what survives it is new.
    struct ClipPaintEntry {
        SLink       *lk;
        offset_t     startTime;
        int          childIndex;
        std::uintptr_t objId;
    };
    std::vector<ClipPaintEntry> entries;
    entries.reserve( (size_t) getTrack().childCount() );
    for( SLink *lk : getTrack().childLinks() ) {
        // Child tracks are summed into this (folder) track's audio but they are
        // their own lanes — don't draw them as clips here. The lane shows only
        // this track's own clips.
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) continue;
        if( !lk->hasStartTime() ) continue;
        if( !lk->getSObject().hasDuration() ) continue;
        const offset_t startTime = lk->getStartTime();
        const length_t length = lk->getSObject().getDuration();
        if( startTime >= rightTime || (startTime+length)<leftTime ) continue;
        ClipPaintEntry e;
        e.lk = lk;
        e.startTime = startTime;
        e.childIndex = getTrack().indexOfChild( lk );
        e.objId = reinterpret_cast<std::uintptr_t>( &lk->getSObject() );
        entries.push_back( e );
    }
    std::sort( entries.begin(), entries.end(),
        []( const ClipPaintEntry &a, const ClipPaintEntry &b ) {
            if( a.startTime != b.startTime ) return a.startTime < b.startTime;
            if( a.childIndex != b.childIndex ) return a.childIndex < b.childIndex;
            return a.objId < b.objId;
        } );

    for( const ClipPaintEntry &entry : entries ) {
        SLink *lk = entry.lk;
        bool isSelected = SAppContext::get().isSLinkSelected( lk );
        const offset_t startTime = entry.startTime;
        const length_t length = lk->getSObject().getDuration();
        double relStart = (double)startTime-(double)leftTime;
        double relEnd = (double)(startTime+length)-(double)leftTime;
        double startX = ((double)visibRect.x())
            +( relStart*((double)(visibRect.width()))
               / ((double)(rightTime-leftTime)) );
        if( startX-visibRect.x()>visibRect.width() ) continue;
        double endX = ((double)visibRect.x())
            +( relEnd*((double)(visibRect.width()))
               / ((double)(rightTime-leftTime)) );
        if( endX<visibRect.x() ) continue;

        // D10 (proposal 41): paint the MATERIAL, not the window. The clip
        // body used to be one opaque fillRect spanning the whole window, so a
        // later clip's EMPTY regions painted over an earlier clip's MATERIAL
        // and the reader saw a hole where audio is sounding (an asset over a
        // container whose own children don't fill the whole placed window is
        // the shape reachable today — no SLaneFragment placement exists yet).
        // No transparency anywhere: this is clipping, not alpha. Per column,
        // fill only where SObjectRenderer::collectEnvelope says material
        // exists (min!=0 || max!=0, the same "silence draws nothing" proxy
        // drawChildSumOverlay() above already uses, proposal 39 M3's
        // discipline); leave a gap column unpainted so whatever was drawn
        // beneath it — an earlier clip, the lane background, the folder-sum
        // overlay — shows through. An object whose renderer does not support
        // collectEnvelope at all (the default false — an event clip has no
        // waveform) falls back to the ORIGINAL solid fill: "unknown material"
        // must never read as "no material".
        const int bodyLeft  = (int) startX;
        const int bodyWidth = (int)( endX - startX );
        const QColor bodyColor( 160, 160, 160 );
        SObjectRenderer *rndr = lk->getSObject().getInlineRenderer();
        bool paintedByMaterial = false;
        if( rndr && bodyWidth >= 1 ) {
            SEnvelopeWindow bodyWin;
            bodyWin.leftTime  = ctx.getTimeOf( bodyLeft );
            bodyWin.rightTime = ctx.getTimeOf( bodyLeft + bodyWidth );
            bodyWin.width     = bodyWidth;
            QVarLengthArray<preview_t> pv( bodyWidth );
            if( rndr->collectEnvelope( *lk, bodyWin, pv.data() ) ) {
                paintedByMaterial = true;
                p.setPen( bodyColor );
                const int top = visibRect.y();
                const int bottom = visibRect.y() + visibRect.height() - 1;
                for( int i = 0; i < bodyWidth; ++i ) {
                    if( !pv[i].min && !pv[i].max ) continue;   // gap: leave it
                    const int x = bodyLeft + i;
                    p.drawLine( x, top, x, bottom );
                }
            }
        }
        if( !paintedByMaterial ) {
            p.fillRect( bodyLeft, visibRect.y(), bodyWidth, visibRect.height(),
                        bodyColor );
        }

        if( isSelected ) {
            p.setPen( QColor( 255, 255, 255 ) );
            p.drawRect( (int)startX+1, visibRect.y()+1, 
                        (int)(endX-startX)-2, visibRect.height()-2 );
            p.setPen( QColor( 0, 0, 0 ) );
            p.drawRect( (int)startX+2, visibRect.y()+2, 
                        (int)(endX-startX)-4, visibRect.height()-4 );
        }
        // Now draw the inner of the object.
        InlineRenderContext myctx( ctx, p );
        QRect vr( (int)startX+1, visibRect.y()+1, 
                  (int)(endX-startX)-2, visibRect.height()-2 );
        if( vr.topLeft().x()<visibRect.x() ) vr.setLeft( visibRect.x() );
        if( vr.bottomRight().x()>visibRect.bottomRight().x() ) 
            vr.setRight( visibRect.bottomRight().x() );
        if( vr.width()<1 ) continue;
        myctx.setVisibRect( vr );
        //qWarning( "lk is $%08x.\n", (unsigned ) lk );
        // rndr was already fetched above, for the material-aware body fill.
        if( rndr ) {
            rndr->draw( *lk, myctx );
        }
        // Draw the number of links into the upper right.
        {            
            p.setPen( QColor( 0,0,0 ) );
            p.drawText( vr, Qt::AlignTop|Qt::AlignRight,
                        QString::number( lk->getSObject().getNReferences() ) );
        }
    }

    // Proposal 40 M2: the compliance heatmap band, drawn LAST -- see
    // drawFeelFlowBand()'s doc for why it cannot share the folder-sum
    // overlay's earlier slot.
    drawFeelFlowBand( p, visibRect, ctx, getTrack() );
}

/**
 * Return the absolute time (in samples, for now) of the given x position.
 * This depends on the zoom factor of this model.
 */
offset_t STrackRendererInline::InlineRenderContext::getTimeOf( int x ) const
{
    return parentRC_.getTimeOf( x );
}

STrackRendererInline::InlineRenderContext::~InlineRenderContext()
{
}

STrackRendererInline::InlineRenderContext::InlineRenderContext( 
    SRenderContext &par, QPainter &painter )
    : SRenderContext( painter ),
      parentRC_( par )
{    
}


STrackRendererInline::STrackRendererInline( STrack &track )
    : SObjectRenderer( (SObject &)track )
{
}

STrackRendererInline::~STrackRendererInline()
{
}
