#include <stdio.h>
#include <qpainter.h>
#include <qobject.h>
#include <QFileInfo>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QVarLengthArray>

#include <algorithm>
#include <vector>

#include "app/model/sappcontext.h"
#include "app/model/sclipcolors.h"
#include "app/model/sclipwindow.h"
#include "app/model/sexternfile.h"
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

    // Determine base color (depends on selection state).
    //
    // NEUTRAL FOR EVERY TRACK, and half the brightness it used to be. The
    // per-track colour now lives on the CLIPS (app/model/sclipcolors.h), and a
    // lane background tinted with it as well would compete with them: the point
    // of dropping the borders is that a clip separates from its lane by TONE, so
    // the lane has to be the quiet one. The halving is the same 50 % the grid
    // and the separators took.
    QColor baseColor = isTrackSelected ? QColor( 30, 45, 65 ) : QColor( 20, 35, 50 );

    // Apply track state modifiers (muted, solo, armed for recording), with the
    // OFFSETS HALVED to match the halved base. They are absolute level counts
    // tuned against the old, twice-as-bright lane; applied unscaled here they
    // made a MUTED lane come out LIGHTER than an unmuted one (#27313C against
    // #142332). The HEAD keeps the full offsets -- it did not halve.
    STrackColorModifier mod = STrackColorModifier::fromTrackState( track )
                                 .withScaledOffsets( 0.5f );
    return mod.apply( baseColor );
}

// THE TAG CHIP (proposal 41 D12-D14, M6) -- see the header doc for the
// contract. The three statics below are the shared decision between the
// clip loop's chip drawing (further down in draw()) and the pixel gate
// (preview_envelope_test.cpp section 7).

QColor STrackRendererInline::tagChipColor( const QColor &laneFill )
{
    // Strictly DARKER than the fill it derives from -- see the header doc
    // for the measured luminance ranges this holds over.
    return laneFill.darker( 160 );
}

QString STrackRendererInline::tagDensityText( const QString &fullName,
    const QFontMetrics &fm, int availTextWidthPx, bool *cut )
{
    if( cut ) *cut = false;
    if( fullName.isEmpty() ) return QString();
    if( availTextWidthPx < 1 ) {
        if( cut ) *cut = true;
        return QString();
    }

    // D12: cap at kTagMaxChars BEFORE any pixel fitting -- a cap on the
    // identifying text itself, independent of how much room the clip has.
    QString capped = fullName;
    if( capped.length() > kTagMaxChars )
        capped = capped.left( kTagMaxChars - 1 ) + QChar( 0x2026 );   // "..."
    if( cut && capped != fullName ) *cut = true;

    if( fm.horizontalAdvance( capped ) <= availTextWidthPx ) return capped;

    // ELIDED: fewer characters than the cap, plus an ellipsis, iff at least
    // one character plus the ellipsis fits -- Qt's own elidedText already
    // refuses (returns "") when it does not, which is exactly the next rung
    // down the ladder (CHIP ONLY -- the caller's business, not this
    // function's: it has no notion of a chip, only of text).
    const QString elided = fm.elidedText( capped, Qt::ElideRight, availTextWidthPx );
    if( cut && !elided.isEmpty() ) *cut = true;
    return elided;
}

QString STrackRendererInline::tagFullText( SObject &clipObject )
{
    if( SClipWindow *win = SClipWindow::of( &clipObject ) ) {
        if( SExternFile *xf = dynamic_cast<SExternFile *>( &win->windowContent() ) ) {
            const QString fn = xf->getFileName();
            if( !fn.isEmpty() ) return QFileInfo( fn ).fileName();
        }
    }
    // Container-backed (asset/fragment) or anything else with no file
    // behind it: the clip's own name -- exactly what the old bottom-right
    // label read for a container-backed SCut (cut.getSName()), generalised
    // to whatever window kind is windowing it.
    return clipObject.getSName();
}

// proposal 41 D15/M7: THE CHIP'S GEOMETRY, factored out of draw()'s own
// chip-drawing block below so a hit test (SMVActualView::tagHitTestAt) asks
// the SAME question the paint path answers, rather than re-deriving the
// padding/minimum-width constants a second time and risking the two
// disagreeing about where the handle actually is.
QRect STrackRendererInline::tagChipRect( SObject &clipObject, const QRect &vr,
                                         QString *outDrawnText )
{
    if( outDrawnText ) outDrawnText->clear();

    const QString fullName = tagFullText( clipObject );
    const int kTagPadX = 3;
    const int kTagPadY = 1;
    const int kTagMinChipW = 6;    // "chip only": still reads as a mark
    const int kTagMinHeightPx = 8; // lane too short: draw nothing at all
    if( fullName.isEmpty() || vr.height() < kTagMinHeightPx || vr.width() < 1 )
        return QRect();

    QFont tagFont = QGuiApplication::font();
    tagFont.setPointSize( 7 );   // matches scutLoopHandleRect's
                                  // "one character high"
    QFontMetrics fm( tagFont );
    const int chipH = qMin( vr.height(),
        qMax( fm.height() + 2 * kTagPadY, kTagMinHeightPx ) );
    const int chipTop = vr.bottom() - chipH + 1;

    const QString drawn = tagDensityText( fullName, fm,
        vr.width() - 2 * kTagPadX, nullptr );
    int chipW = 0;
    if( !drawn.isEmpty() ) {
        chipW = qMin( vr.width(), fm.horizontalAdvance( drawn ) + 2 * kTagPadX );
    } else if( vr.width() >= kTagMinChipW ) {
        chipW = qMin( kTagMinChipW, vr.width() );   // CHIP ONLY
    }
    // else: NOTHING -- not even the chip fits.
    if( chipW < 1 ) return QRect();

    if( outDrawnText ) *outDrawnText = drawn;
    return QRect( vr.left(), chipTop, chipW, chipH );
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

    // THE TRACK'S PALETTE ENTRY, resolved ONCE for the whole lane rather than
    // once per clip: the auto index walks the project lanes, which is O(lanes)
    // and would otherwise be paid by every clip on every repaint.
    int colorIndex = getTrack().colorIndex();
    if( colorIndex < 0 ) {
        SProject *proj = SAppContext::get().getCurrentProject();
        SObject *root = proj ? proj->getRootComponent() : nullptr;
        colorIndex = root ? sclipcolors::autoIndexForLane( *root, getTrack() ) : 0;
    }
    const bool laneMuted = getTrack().isMuted();

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
        entries.push_back( e );
    }
    std::sort( entries.begin(), entries.end(),
        []( const ClipPaintEntry &a, const ClipPaintEntry &b ) {
            if( a.startTime != b.startTime ) return a.startTime < b.startTime;
            // childIndex is a POSITION in the lane's child list, so it is
            // unique across these entries and (startTime, childIndex) is
            // already a TOTAL order — there is no third key and there must
            // not be one. Proposal 41 D11 originally said "child index, then
            // object id"; an SObject's "id" in this tree is its ADDRESS
            // (slink.cpp serializes it that way), so an address comparison
            // here would be non-deterministic run to run. The tiebreak is
            // contractual (it is the only case that can hide the bottom-left
            // corner M6 pins a drag handle to), so it may not rest on
            // anything the allocator chooses.
            return a.childIndex < b.childIndex;
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
        // No transparency anywhere: this is clipping, not alpha. The per-column
        // rule and the "unknown material must never read as no material"
        // fallback both live in fillBodyByMaterial() (app/model/
        // sobjectrenderer.h), because the TAKE LANES paint the same way and a
        // second copy is exactly how the same grey came to mean "material" on
        // this lane and "window" on the take lane directly below it.
        // THE BODY COLOUR IS THE TRACK'S, in the variant this clip's state
        // asks for (app/model/sclipcolors.h). It used to be one fixed grey for
        // every clip in every project.
        const QColor bodyColor =
            sclipcolors::body( colorIndex, isSelected, laneMuted );
        const QRect bodyRect( (int) startX, visibRect.y(),
                              (int)( endX - startX ), visibRect.height() );
        SObjectRenderer *rndr = lk->getSObject().getInlineRenderer();
        const bool paintedByMaterial = rndr
            && fillBodyByMaterial( p, bodyRect, bodyColor, ctx,
                   [&]( const SEnvelopeWindow &w, preview_t *o ) {
                       return rndr->collectEnvelope( *lk, w, o );
                   } );
        if( !paintedByMaterial )
            p.fillRect( bodyRect, bodyColor );

        // NO SELECTION BORDER. Selection is carried by the BRIGHTER body
        // variant above -- the white rect plus the black rect inside it were
        // two drawn lines doing what a tone change now does, and they are
        // exactly the kind of chrome this pass exists to remove. The clip is
        // still inset by one row top and bottom (`vr` below), so the lane
        // background shows between stacked lanes as a margin rather than as a
        // border on the clip.
        // Now draw the inner of the object.
        InlineRenderContext myctx( ctx, p );
        QRect vr( (int)startX+1, visibRect.y()+1, 
                  (int)(endX-startX)-2, visibRect.height()-2 );
        if( vr.topLeft().x()<visibRect.x() ) vr.setLeft( visibRect.x() );
        if( vr.bottomRight().x()>visibRect.bottomRight().x() ) 
            vr.setRight( visibRect.bottomRight().x() );
        if( vr.width()<1 ) continue;
        myctx.setVisibRect( vr );
        // The waveform / note colour the CONTENT will draw itself in: same hue,
        // near-grey and much lighter, so it reads on any anchor. The child
        // renderers never see a track (they may not include app/objects/track),
        // so it travels on the context.
        {
            SClipColors cc;
            cc.body = bodyColor;
            cc.wave = sclipcolors::wave( colorIndex, isSelected, laneMuted );
            myctx.setClipColors( cc );
        }
        //qWarning( "lk is $%08x.\n", (unsigned ) lk );
        // rndr was already fetched above, for the material-aware body fill.
        if( rndr ) {
            rndr->draw( *lk, myctx );
        }
        // Draw the number of links into the upper right.
        {
            // In the CONTENT colour, not black. Black was legible only while
            // every clip body was a light grey; on a mid-tone anchor it
            // disappears.
            p.setPen( myctx.clipColors().wave );
            p.drawText( vr, Qt::AlignTop|Qt::AlignRight,
                        QString::number( lk->getSObject().getNReferences() ) );
        }

        // D12-D14 (proposal 41 M6): THE TAG CHIP. One solid opaque chip,
        // bottom-left corner of `vr` -- the same inset rect the content,
        // the warp markers, the pitch badge and the gain envelope
        // (SCutRendererInline) already draw into, so the tag lines up with
        // everything else drawn on this clip. Bottom because warp markers
        // own the top edge; left because D11's paint order guarantees that
        // corner is never covered except at an exact start-time tie, which
        // the childIndex tiebreak above makes deterministic rather than
        // flaky. It REPLACES the old bottom-right container-asset label
        // (SCutRendererInline used to draw cut.getSName() there) -- one
        // mechanism, two text sources (tagFullText: the fragment/asset name
        // for a container-backed clip, the file name for a plain one).
        // proposal 41 D15/M7: geometry now comes from tagChipRect() -- the
        // SAME function SMVActualView::tagHitTestAt() calls for the hit
        // test, so paint and hit-test can never independently drift apart
        // on where the chip actually is. This block only draws.
        {
            QString drawnTagText;
            const QRect chipRect = tagChipRect( lk->getSObject(), vr, &drawnTagText );
            if( !chipRect.isEmpty() ) {
                const QColor chipColor = tagChipColor( finalColor );
                p.fillRect( chipRect, chipColor );
                if( !drawnTagText.isEmpty() ) {
                    QFont tagFont = QGuiApplication::font();
                    tagFont.setPointSize( 7 );   // matches tagChipRect's own
                                                  // font, and scutLoopHandleRect's
                                                  // "one character high"
                    QFont oldFont = p.font();
                    p.setFont( tagFont );
                    p.setPen( QColor( 255, 255, 255 ) );
                    p.drawText( chipRect.adjusted( 3, 0, -3, 0 ),
                               Qt::AlignVCenter | Qt::AlignLeft, drawnTagText );
                    p.setFont( oldFont );
                }
                // D14: "the cap is announced" -- whenever what is drawn is
                // not the true full name (the 12-char cap, further pixel
                // elision, or the chip-only/nothing rungs where drawn is
                // empty), the full name belongs on a tooltip. The canvas
                // has no per-clip widget to hang a QToolTip off, so that is
                // wired at the arranger's own QEvent::ToolTip handler
                // (SMVActualView::event), which recomputes the identical
                // answer through tagFullText()/tagDensityText() rather than
                // needing this draw to cache anything.
            }
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
    // Inherit the clip colours the parent resolved (app/model/sclipcolors.h);
    // the clip loop overwrites them per clip before it calls a renderer.
    setClipColors( par.clipColors() );
}


STrackRendererInline::STrackRendererInline( STrack &track )
    : SObjectRenderer( (SObject &)track )
{
}

STrackRendererInline::~STrackRendererInline()
{
}
