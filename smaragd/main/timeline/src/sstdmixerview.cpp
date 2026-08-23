
#include <stdlib.h>

#include <algorithm>
#include <cmath>

#include <QtDebug>
#include <qwidget.h>
#include <qpushbutton.h>
#include <qevent.h>
#include <qpainter.h>
#include <qmenu.h>
#include <qfiledialog.h>
#include <QFileInfo>
#include <qscrollbar.h>
#include <QSignalBlocker>
#include <qlayout.h>
#include <qmessagebox.h>
#include <qinputdialog.h>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMainWindow>
#include <QStatusBar>
#include <QApplication>
#include <QToolTip>

#include "tw/sources/twwavinput.h"
#include "tw/playback/twspeaker.h"
#include "tw/core/twlog.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/timeline/sstdmixerview.h"
#include "app/timeline/ssubmit.h"
#include "app/shell/sviewtabs.h"
#include "app/timeline/strackheaderresizer.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackrndrinline.h"
#include "app/model/sclipcolors.h"
#include "app/model/sobjectrenderer.h"
#include "app/objects/wave/splainwave.h"
#include "app/model/slink.h"
#include "app/model/sclipwindow.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/stakehelpers.h"
#include "app/objects/cut/ssetclipfadeaction.h"
#include "app/objects/cut/scompmapactions.h"
#include "app/objects/cut/swarpmarkeractions.h"
#include "app/model/sexternfile.h"
#include "app/objects/cut/scutrndrinline.h"   // loop-marker handle geometry
#include "sautomationlane.h"                   // proposal 37 P6: automation lanes
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/shell/ssettings.h"
#include "app/servicesui/soptions.h"
#include <QWheelEvent>
#include <QCursor>
#include <QPixmap>
#include <QPolygon>
#include <QPen>
#include "app/timeline/ssmvmixercontrol.h"
#include "app/objects/mixer/saddtrackaction.h"
#include "app/objects/mixer/spackselectionaction.h"
#include "app/objects/mixer/sunpackclipsaction.h"
#include "app/objects/mixer/smovetrackaction.h"
#include "app/objects/mixer/sreparenttrackaction.h"
#include "app/objects/mixer/sremovetrackaction.h"
#include "app/objects/track/smoveclipaction.h"
#include "app/objects/cut/ssplitclipaction.h"
#include "app/objects/cut/sduplicateclipaction.h"
#include "app/selection/ssetselectionaction.h"
#include "app/objects/cut/sresizeclipaction.h"
#include "app/objects/mixer/screateassetaction.h"
#include "app/objects/mixer/sextractarrangementaction.h"
#include "app/objects/mixer/splaceassetaction.h"
#include "app/objects/cut/saddsampleaction.h"
#include "app/objects/cut/sremovesampleaction.h"
#include "app/objects/cut/stakestack.h"
#include "app/objects/cut/sselecttakeaction.h"
#include "app/objects/cut/ssetpitchaction.h"
#include "app/objects/midi/smidiclipactions.h"
#include "app/objects/midi/smidieventactions.h"
#include "app/actions/scompositeaction.h"
#include "app/objects/track/strackpath.h"
#include "app/objects/mixer/sremoveassetplacementaction.h"
#include "app/media/smediadrop.h"
#include "app/media/smediaref.h"
#include "app/model/splacements.h"
#include "app/model/splayheadmap.h"
#include "app/actions/sactionhistory.h"
#include <QFrame>
#include <QUndoStack>
#include <qaction.h>
#include <QKeySequence>

// Icons needed here

#include "pix/zoomin.xpm"
#include "pix/zoomout.xpm"

// The timeline's "primary" editing modifier is Qt::ControlModifier. On macOS Qt
// maps ControlModifier to the Command (⌘) key by DEFAULT (the physical Control
// key becomes Qt::MetaModifier) — the same swap that makes QKeySequence("Ctrl+S")
// mean ⌘S. So this one predicate drives the editing gestures with ⌘ on macOS and
// Ctrl on Windows/Linux, no platform branch. The physical Control key
// (Qt::MetaModifier on macOS) is deliberately left to the OS: it is the
// secondary-click (right-click) key and the accessibility screen-zoom scroll key.
static inline bool hasPrimaryMod( Qt::KeyboardModifiers m ) { return m & Qt::ControlModifier; }

// QApplication::activeWindow() needs the window to have been ACTIVATED by the
// window system, which never happens in a headless --test-case run (main.cpp
// never calls SMainWindow::show()/showMaximized() there) - it would return
// null and silently no-op the double-click-opens-the-editor gesture below.
// Same lookup drag-clip-edge and the event-editor test entry points already
// use for exactly this reason (testkit's local mainWindow() helper).
static SMainWindow *findMainWindow()
{
    for( QWidget *w : QApplication::topLevelWidgets() ) {
        if( SMainWindow *win = qobject_cast<SMainWindow*>( w ) ) return win;
    }
    return nullptr;
}

// True if `t` has at least one child TRACK (as opposed to only clips) --
// i.e. it is a folder that can be expanded/collapsed. Used by the
// double-click container resolve below to tell a foldable folder track from
// a plain leaf track referenced as an asset window.
static bool hasChildTracks( STrack *t )
{
    if( !t ) return false;
    for( SLink *lk : t->childLinks() )
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) return true;
    return false;
}

// The take stack a link's clip represents OR WRAPS. proposal 17's modern
// shape places a take column directly (SLink -> STakeStack); an older shape
// a real saved project can still carry is SLink -> SCut whose CONTENT is
// the STakeStack (found while fixing the double-click "blueish clip does
// nothing" bug -- rendering AND every take-lane gesture below silently
// no-op on that shape without this resolve, which is why every
// dynamic_cast<STakeStack*>( &lk->getSObject() ) that answers "does THIS
// LINK carry take lanes" goes through here instead of repeating the cast).
// Null for a plain clip with no take column at all.
//
// IT IS NOW ONE SPELLING WITH THE ACTIONS, and it had to become one. The
// comment here used to say this resolve was "deliberately scoped to take-lane
// RENDERING/INTERACTION only", on the grounds that objects/cut's actions ask a
// different, per-action question. For most of them that is still true — a
// `set-clip-volume` on a wrapped column really does mean the WRAPPER. But two
// of them ask EXACTLY this question, because they exist to serve this UI:
// `select-take` (which take is audible) and the take-addressed half of
// `resize-clip` (slip THIS take). Both resolved DIRECT-only while this
// unwrapped, so a click on a wrapped column's take lane was REFUSED and an
// Alt-drag on one wrote the take's window onto the wrapper. The resolve now
// lives in `stakes::columnOfLink` where both sides can reach it; anything
// finer is still each action's own business.
static STakeStack *takeStackOfLink( SLink *lk )
{
    return stakes::columnOfLink( lk );
}

// THE WINDOW A CLIP GESTURE EDITS — resolved ONCE per gesture, proposal 42 M1.
//
// A placement's object is one of three things, and the gesture layer used to
// force it to be the first by REWRITING THE TREE (`ensureSCut`, below):
//
//   * an `SCut`            — the common case, and the only one that used to work
//   * another `SClipWindow` — an `SMidiCut`, or an `SCut` WRAPPING a take column
//   * a take COLUMN        — `SLink -> STakeStack`, what `add-take` builds
//
// The rule is one line: **a gesture edits the placement's own WINDOW when it
// has one, and the ACTIVE TAKE of a column when it does not.** Order matters —
// a wrapper IS a window, so a wrapped column's border drag edits the WRAPPER,
// which is exactly what `resize-clip` with `take < 0` already does.
//
// `column` is non-null only for the DIRECT shape, and is what an EXTENT edit
// writes through to (`stakestack.h` invariant 1: all takes share the column's
// duration). `cut` is the same object as `win` whenever the window is audio,
// so every existing gesture branch keeps its `SCut` fast path untouched.
struct ClipEditTarget {
    SClipWindow *win    = nullptr;
    STakeStack  *column = nullptr;
    SCut        *cut    = nullptr;
};

static ClipEditTarget clipEditTargetOf( SLink *lk )
{
    ClipEditTarget t;
    if( !lk ) return t;
    SObject &obj = lk->getSObject();
    t.win = SClipWindow::parametersOf( obj );   // the rule, said once (M2)
    if( !SClipWindow::of( &obj ) ) t.column = stakes::columnOfLink( lk );
    if( t.win ) t.cut = dynamic_cast<SCut *>( &t.win->asObject() );
    return t;
}

// NO EXTENT WRITE-THROUGH DURING THE DRAG, deliberately. On a direct column
// the gesture edits the ACTIVE take, and `STakeStack::getDuration()` delegates
// to exactly that take — so the column's drawn extent (and every take ROW's,
// which `drawTakeLane` sizes from the LINK's object) follows live with no
// second write. `stakestack.h` invariant 1 is restored at the RELEASE, by
// `SResizeClipAction`'s `applyWindowAll`, which is the one place length ops on
// a column live. Writing through per mouse-move would put the inactive takes
// ahead of the revert the release performs before it submits.

// The wheel response AT 100 % SENSITIVITY. SOpt::WheelSensitivityPct scales all
// four together (see loadWheelConfig); these stay the reference point, so the
// default feel is stated once and can still be read off here.
//
// Vertical wheel-scroll (fix/arranger-ui-fixes C): PIXEL-granular, a fraction
// of the CURRENT base lane height per notch (a standard mouse notch is 120
// angleDelta units) — roughly a third of a lane per notch at the default
// height, which reads as a similar feel to the old "one lane per 5 notches"
// while landing on any pixel, including a partially visible top row.
static constexpr double SMV_WHEEL_VSCROLL_FRACTION = 1.0 / 3.0;
// Zoom is multiplicative, so its sensitivity is an EXPONENT rather than a factor:
// n notches at s == 1 must be the same zoom as one notch at s == n, which only
// pow() gives. 1.2x per notch on the time axis, 1.5x on track height.
static constexpr double SMV_WHEEL_ZOOM_H_BASE = 1.2;
static constexpr double SMV_WHEEL_ZOOM_V_BASE = 1.5;

// The BASE lane height a fresh view starts at, in pixels. It is not persisted
// anywhere (unlike the horizontal zoom/pan, which ride in the project's
// property dict), so every view is built at this height and the wheel zoom
// works out from it. 49 px is one toggle row plus one fader row with the head
// still legible at the compact density - what the arranger is actually used
// at, rather than the 100 px it started life with.
static constexpr int SMV_DEFAULT_TRACK_HEIGHT = 49;

void SMVActualView::setSecondWidth( double w )
{
    if( w<0.000001 ) w=0.000001;
    secondWidth_ = w;
    // Keep the pixel origin consistent with the (unchanged) left time offset, so
    // the left edge stays put on zoom rather than drifting.
    int srate = smv_.model_ ? smv_.model_->getProject().getSRate() : 48000;
    upperLeftX_ = (int)( ((double)upperLeftOffset_)/srate*secondWidth_ );
    smv_.viewResized();
    update();
    // The zoom is part of the px<->frame mapping, and since proposal 37 P4 the
    // event editor's axis mirrors that mapping - so the signal that was a FIXME
    // here is now load-bearing.
    emit secondWidthChanged( secondWidth_ );
}

void SMVActualView::setTrackHeight( int h )
{
    if( h<6 ) h = 6;
    trackHeight_ = h;
    // The base height feeds every lane's height (through its track's scale),
    // so the row geometry has to be rebuilt before anything is placed. Scroll
    // is PIXEL-granular (fix/arranger-ui-fixes C), and a row boundary does not
    // survive a height change, so re-anchor by FRACTION of the total height
    // rather than by row — same reasoning as the SStdMixerView call sites
    // that do this (setTrackHeightScale, onArrangementChangedRows,
    // refreshTrackTree).
    const int oldTotal = smv_.totalRowsHeight();
    const double frac = ( oldTotal > 0 ) ? (double) upperLeftY_ / (double) oldTotal : 0.0;
    smv_.rebuildRowGeometry();
    smv_.reanchorScrollByFraction( frac );   // re-anchors + relayouts heads
    smv_.viewResized();
    update();
    // FIXME: Emit signal?
}

// Clamp a requested top row into range. Empty view -> 0.
static idx_t clampTopRow( idx_t topOffset, int nRows )
{
    if( nRows<=0 ) return 0;
    if( topOffset<0 ) return 0;
    if( topOffset>=nRows ) return nRows-1;
    return topOffset;
}

void SMVActualView::setUpperLeft( offset_t leftOffset, idx_t topOffset )
{
    setTopOffset( topOffset );
    setLeftOffset( leftOffset );
    // FIXME: Blitting
    update();
}

void SMVActualView::setLeftOffset( offset_t leftOffset )
{
    if( upperLeftOffset_ == leftOffset ) return;
    upperLeftOffset_ = leftOffset;
    int srate = smv_.model_ ? smv_.model_->getProject().getSRate() : 48000;
    int newUpperLeftX = ((int)((((double)leftOffset)/srate)*secondWidth_));
    const bool pixelChanged = ( upperLeftX_ != newUpperLeftX );
    upperLeftX_ = newUpperLeftX;
//    qWarning( "SMVActualView::setLeftOffset(): leftOffset = %d:%d, upperLeftX_ = %d",
//              (int)leftOffset, (int)(leftOffset>>32), upperLeftX_ );
    // FIXME: Blitting
    // Emitted whenever the FRAME offset changed, even when it rounds to the
    // SAME pixel column (e.g. a fine wheel pan at a low zoom level). This used
    // to return above, before the emit, whenever the pixel column matched —
    // which meant the scrollbar sync (avLeftOffsetChanged) and the view-state
    // save (saveViewStateToProject) never saw the change either
    // (fix/arranger-ui-fixes B4ii). Only the repaint is skippable when the
    // screen would look identical.
    emit leftOffsetChanged( leftOffset );
    if( pixelChanged ) update();
}

void SMVActualView::setTopPixel( int y )
{
    // AUTHORITY for vertical scroll (fix/arranger-ui-fixes C). Clamped to the
    // same range the real scrollbar's maximum() computes
    // (SStdMixerView::verticalScrollMaximum()) so this can never show blank
    // space below the content, whatever asked for it.
    const int availPx = qMax( 0, height() - SMV_TIME_RULER_HEIGHT );
    const int maxY = qMax( 0, smv_.totalRowsHeight() - availPx );
    if( y < 0 ) y = 0;
    if( y > maxY ) y = maxY;
    upperLeftY_ = y;
    topRow_ = smv_.rowAtLaneY( y );
    if( topRow_ < 0 ) topRow_ = 0;   // empty view (or the clamp above landed past the last row)
    smv_.layoutControlColumn();
    // FIXME: Blitting
    // FIXME: Signal
    update();
}

void SMVActualView::setTopOffset( idx_t topOffset )
{
    // Thin row->pixel wrapper (fix/arranger-ui-fixes C). Kept for the testkit
    // (tkSetTopRow) and the row-anchored re-anchor call sites; setTopPixel()
    // above is the authority now.
    const idx_t row = clampTopRow( topOffset, smv_.rowCount() );
    setTopPixel( (int) smv_.rowTop( (int) row ) );
}

// --- lane geometry ------------------------------------------------------
// Everything positional in the canvas goes through these three. They are the
// view-space face of SStdMixerView's row geometry: ruler band added, vertical
// scroll subtracted. The control column applies the very same offsets (see
// SStdMixerView::controlYOfRow), which is what keeps heads and lanes glued
// together no matter what changed the geometry.

int SMVActualView::laneTop( int row ) const
{
    return SMV_TIME_RULER_HEIGHT + smv_.rowTop( row ) - (int) upperLeftY_;
}

int SMVActualView::laneHeight( int row ) const
{
    return smv_.rowHeight( row );
}

int SMVActualView::lanesBottom() const
{
    return SMV_TIME_RULER_HEIGHT + smv_.totalRowsHeight() - (int) upperLeftY_;
}

int SMVActualView::rowAtViewY( int y ) const
{
    // A press in the ruler band belongs to the topmost visible lane — the
    // behaviour every click handler has always had (they clamped y to 0).
    int laneY = y - SMV_TIME_RULER_HEIGHT;
    if( laneY < 0 ) laneY = 0;
    return smv_.rowAtLaneY( laneY + (int) upperLeftY_ );
}

int SMVActualView::getXPosOfOffset( offset_t off ) const
{
    int srate = smv_.model_ ? smv_.model_->getProject().getSRate() : 48000;
    return ((int)((((double)off)/srate)*secondWidth_))-upperLeftX_;
}

// Where the playhead is in THIS view's root (proposal 09 §15). See the comment
// on LocalPlayhead in the header, and splayhead::derivedPos for the walk and
// the three decisions it encodes.
//
// The master view takes the early return, so nothing about the master's
// playhead changed with this feature — which is what makes the whole existing
// suite the gate for "nothing moved".
SMVActualView::LocalPlayhead SMVActualView::localPlayhead() const
{
    const offset_t masterPos = SApplication::app().getGlobalLocatorPos();
    const QString root = smv_.rootName();
    if( root.isEmpty() ) return { masterPos, true };

    if( masterPos == lastWalkMasterPos_ ) return lastWalk_;   // one-repaint memo

    LocalPlayhead lp;
    SProject *project = smv_.model_ ? &smv_.model_->getProject() : nullptr;
    const splayhead::Position d = splayhead::derivedPos( project, root, masterPos );
    if( d.sounding ) {
        lp.pos = d.pos;
        lp.sounding = true;
        // Remember where it was last HEARD, so the moment the placement ends
        // the cursor rests there instead of jumping.
        parkedLocalPos_ = d.pos;
    } else {
        lp.pos = parkedLocalPos_;
        lp.sounding = false;
    }
    lastWalkMasterPos_ = masterPos;
    lastWalk_ = lp;
    return lp;
}

void SMVActualView::globalLocatorMoved( offset_t newPos, offset_t oldPos )
{
    // Qt6 forbids constructing a QPainter on a widget outside paintEvent.
    // Instead, invalidate 3px columns so paintEvent redraws the playhead (see the
    // cursor block at the end of paintEvent).
    QRect myRect = rect();
    int w = myRect.width();
    int h = myRect.height();
    // Not getXPosOfOffset(newPos): in an arrangement tab the cursor is drawn
    // at the DERIVED position, so the column to invalidate is that one.
    // (newPos is still what the memo keys on — SApplication has already
    // published it by the time this slot runs.)
    int newX = getXPosOfOffset( localPlayhead().pos );
    const int cursorWidth = 3;

    // Erase the cursor at the column where it was ACTUALLY last painted, NOT at
    // getXPosOfOffset(oldPos): during playback the RT locator keeps advancing
    // between repaints, so on a manual seek oldPos (the pre-seek atomic value)
    // is not where the line is on screen — invalidating it would leave the old
    // line behind as a ghost. lastPaintedCursorX_ is the ground truth.
    if( newX == lastPaintedCursorX_ && !SApplication::app().isRecordingActive() )
        return;   // cursor stays in the same column: nothing to redraw
    if( lastPaintedCursorX_ >= 0 && lastPaintedCursorX_ < w )
        update( lastPaintedCursorX_ - 1, 0, cursorWidth, h );
    if( newX >= 0 && newX < w ) update( newX - 1, 0, cursorWidth, h );

    // While recording, repaint the whole span the playhead swept so the growing
    // capture region fills in continuously (the 3px cursor columns alone would
    // leave gaps when zoomed in / moving fast).
    if( SApplication::app().isRecordingActive() ) {
        int oldX = getXPosOfOffset( oldPos );
        int lo = ( oldX < newX ? oldX : newX ) - 1;
        int hi = ( oldX < newX ? newX : oldX ) + 1;
        if( lo < 0 ) lo = 0;
        update( lo, 0, ( hi - lo ) + cursorWidth, h );
    }
}

void SMVActualView::armFollowHold()
{
    followHoldArmed_ = true;
    followHoldTimer_.restart();
}

void SMVActualView::followLocator( offset_t newPos, offset_t oldPos )
{
    // Only when enabled, and only for a real advance under playback/recording
    // (this slot is wired to locatorAdvanced, so a manual seek never lands here).
    if( !followPlayhead_ ) return;
    if( newPos == oldPos ) return;

    // AC-g1: a manual scroll suspends re-paging for FOLLOW_HOLD_MS after the
    // last one. followHoldTimer_ is only ever armed from a USER-initiated pan
    // (wheelEvent, the scrollbar's own user-driven signal, drag-scroll-past-
    // the-edge) — never from here or from setLeftOffset() itself, which would
    // let the very re-page the hold exists to delay immediately disarm it.
    if( followHoldArmed_ ) {
        if( followHoldTimer_.elapsed() < FOLLOW_HOLD_MS ) return;
        followHoldArmed_ = false;   // the hold has expired; resume following
    }

    int w = width();
    if( w <= 0 ) return;
    // In an arrangement tab the cursor is at the DERIVED position (proposal 09
    // §15), so following newPos — a MASTER frame — would scroll to a column the
    // cursor is nowhere near. Follow what is actually drawn, and do not follow
    // at all while this root is not being heard: the cursor is resting, and a
    // view that scrolled while nothing moved would be chasing a still line.
    const LocalPlayhead lp = localPlayhead();
    if( !lp.sounding ) return;
    int x = getXPosOfOffset( lp.pos );   // cursor position in view-space pixels

    // Leading-edge zones: last 20% moving forward, first 20% winding backward.
    // Re-page so the cursor lands back near the far side (10% / 90%), leaving
    // room ahead of it. Anything outside these zones is left untouched.
    const double forwardEdge  = 0.80 * w;
    const double backwardEdge = 0.20 * w;
    double targetPx;
    if( newPos > oldPos && x >= forwardEdge )        targetPx = 0.10 * w;
    else if( newPos < oldPos && x <= backwardEdge )  targetPx = 0.90 * w;
    else return;

    // Solve for the left time offset that puts newPos at targetPx:
    //   getXPosOfOffset(newPos) == targetPx
    // (same shape as the zoom-to-cursor re-anchoring in wheelEvent()).
    int srate = smv_.model_ ? smv_.model_->getProject().getSRate() : 48000;
    double ahead = targetPx / secondWidth_ * srate;
    offset_t newLeft = ( (double) newPos > ahead )
                           ? (offset_t)( (double) newPos - ahead ) : 0;
    setLeftOffset( newLeft );   // recomputes upperLeftX_, syncs scrollbar, repaints
}

void SMVActualView::resizeEvent( QResizeEvent * )
{
    smv_.viewResized();
}

/**
 * The actual redraw method of the standard mixer view draws
 * - The track grid
 * - The time grid.
 * After that it calls the tracks' inline renderers.
 */
void SMVActualView::paintEvent( QPaintEvent * )
{
    QPainter p( this );
    QRect myRect = rect();
    if( !smv_.model_ ) {
        p.setPen( QColor( 160, 32, 32 ) );
        p.drawText( myRect, Qt::AlignCenter, "No object assigned to view." );
        return;
    }
//    qWarning( "SStdMixerLength is %d.\n", (int) smv_.model_->getDuration() );
    if( !smv_.rowCount() ) {
        p.setPen( QColor( 160, 32, 32 ) );
        p.drawText( myRect, Qt::AlignCenter, "Mixer contains no tracks." );
        p.fillRect( QRect( 0, 0, myRect.width(), myRect.height() ), QColor( 220, 220, 190 ) );
        return;
    }

    // Above the tracks, render the timescale.

    InlineRenderContext ctx( *this, p );
    p.fillRect( QRect( 0, 0, myRect.width(), SMV_TIME_RULER_HEIGHT ), QColor( 220, 220, 190 ) );
    drawRulerTicks( p, myRect );

    // Fill timeline background with black. This appears as a small line between the individual tracks.
    p.fillRect( QRect( 0, SMV_TIME_RULER_HEIGHT, myRect.width(), myRect.height() - SMV_TIME_RULER_HEIGHT ), 
        QColor( 0x00, 0x00, 0x00 ) 
        );

    // Vertical scroll is pixel-granular (fix/arranger-ui-fixes C), so the
    // first visible row can now be only PARTIALLY on screen — its laneTop()
    // can sit above SMV_TIME_RULER_HEIGHT, even negative. Nothing below ever
    // relied on the first lane starting flush against the ruler before, so
    // clip the lane loop and the record overlay to the band below it; the
    // grid lines and the cursor draw their own bounded rects and need no clip.
    p.save();
    p.setClipRect( 0, SMV_TIME_RULER_HEIGHT, myRect.width(),
                   myRect.height() - SMV_TIME_RULER_HEIGHT );

    // OK, we have tracks (lanes of the flattened tree).
    int nTracks = smv_.rowCount();
    // First lane touching the viewport. With variable heights this is a
    // lookup, not a division — and it is the lane *containing* the scroll
    // offset, so a partially scrolled lane still paints.
    int firstTrack = smv_.rowAtLaneY( (int) upperLeftY_ );
    if( firstTrack < 0 ) firstTrack = nTracks;
    for( int i=firstTrack; i<nTracks; i++ ) {
        const STrackRow *row = smv_.rowAt( i );
        if( !row ) continue;
        int top = laneTop( i );
        if( top >= myRect.height() ) break;    // rest is below the viewport
        int lh = laneHeight( i );
        // All lanes start at x=0 (full width) — the hierarchy is shown by the
        // indented control strips, not the timeline, so editing keeps full width.
        // The lane separator is THE OFFBEAT GRID COLOUR: the two are the same
        // kind of ruling and should read as the same weight. After the halving
        // that is 48; it used to be 96, twice as loud as what it separates.
        p.setPen( QColor( 48, 48, 48 ) );
        ctx.setVisibRect(
            QRect( 0, top+1, myRect.width(), lh-2 ) );
        p.drawLine( 0, top, myRect.bottomRight().x(), top );
        p.drawLine( 0, top+lh-1,
                    myRect.bottomRight().x(), top+lh-1 );
        if( row->subKind == SubLaneKind::Automation ) {
            smv_.automationUi().drawAutomationLane(
                p, *this, *row, QRect( 0, top+1, myRect.width(), lh-2 ) );
        } else if( row->isSubLane() ) {
            // A take lane: take k of every stack on the track (phase 3).
            drawTakeLane( p, *row, i,
                          QRect( 0, top+1, myRect.width(), lh-2 ) );
        } else {
            // A selected lane gets a tinted BACKGROUND (under the clips, which
            // paint over it), so a multi-track selection is legible in the
            // timeline and not only on the heads.
            if( row->track && smv_.getModel()
                && smv_.getModel()->isTrackSelected( row->track ) ) {
                p.fillRect( QRect( 0, top+1, myRect.width(), lh-2 ),
                            QColor( 15, 23, 33 ) );   // halved with the field
            }
            // Draw the track's clips.
            row->track->getInlineRenderer()->draw( *row->link, ctx );
        }
    }

    // While recording, show the in-progress capture as a translucent region that
    // grows with the playhead, on each armed lane (from the record start to the
    // live locator). The real clip — with its waveform — is placed when recording
    // finishes; this is live feedback that something is being captured.
    // Master root only: recordingStartFrame() and the global locator are both
    // MASTER frames, so this overlay would be drawn at meaningless columns in
    // an arrangement tab. Recording into an arrangement is not reachable
    // today (an armed track lives in the master), so there is nothing there
    // to draw rather than something drawn wrongly.
    if( SApplication::app().isRecordingActive() && smv_.rootName().isEmpty() ) {
        int xs = getXPosOfOffset( SApplication::app().recordingStartFrame() );
        int xe = getXPosOfOffset( SApplication::app().getGlobalLocatorPos() );
        if( xe > xs ) {
            for( int i=firstTrack; i<nTracks; i++ ) {
                int top = laneTop( i );
                if( top >= myRect.height() ) break;
                const STrackRow *row = smv_.rowAt( i );
                if( !row || !row->track || !row->track->isArmedForRecording() ) continue;
                p.fillRect( QRect( xs, top+1, xe-xs, laneHeight( i )-2 ),
                            QColor( 220, 40, 40, 70 ) );
            }
        }
    }
    p.restore();

    // Before painting the timegrid, decide, wether the grid elements are not too close
    // together. Grid visibility is a per-project property (toolbar palette / grid action).
    STimeGridSpec tgs = smv_.getTimeGridSpec();
    bool gridOn = smv_.model_->getProject().prop( SProjectProps::GridVisible, true ).toBool();
    if( gridOn && (tgs.getTimeGridWidth()*secondWidth_) > 6.0 ) {
        double a = (double)upperLeftX_;
        int maxX = myRect.width();
        a /= secondWidth_;
        int c = (int) (a / tgs.getTimeGridWidth() );
        // Now, that's the time from the last grid to the top left corner.
        a -= (double)(c * tgs.getTimeGridWidth() );
        a = tgs.getTimeGridWidth() - a;
        // And that's the time of the first grid relative to the start.
        while(true) {
            ++c;
            bool emph = false;
            for( int j=0; j<4; ++j ) {
                int e = tgs.getEmphasizeGrids( j );
                if( e<=0 ) break;
                if( (c%e)==0 ) {
                    emph=true;
                    break;
                }
            }
            int x = (int) (a * secondWidth_);
            if( x>=maxX ) break;
            // Pick the pen per line: emphasized (bar) lines dark, the rest light.
            // (The old code only set the light pen *after* the first bar line, so
            // the first few lines were wrongly drawn in the bar colour.)
            // THE SWAP: the emphasized (bar) line is now the LIGHT one and every
            // other line is dark. The downbeat is what the eye should find, and
            // the subdivisions should recede into the background instead of
            // ruling it into boxes. Both are then half the brightness they were,
            // with the rest of the field.
            //
            // IT ALSO ENDS A COLLISION. The subdivision line used to be drawn in
            // EXACTLY the old clip-body grey, so a grid line was a full-height
            // column of "material" to every pixel gate -- which is why those
            // cases have to `grid-disable` before they can measure anything, and
            // why describeTakeLane settles a pending repaint first. Nothing in
            // the grid is a clip colour any more.
            p.setPen( emph ? QColor( 80, 80, 80 ) : QColor( 48, 48, 48 ) );
            p.drawLine( x, SMV_TIME_RULER_HEIGHT, x, lanesBottom() );
            a += tgs.getTimeGridWidth();
        }
    }
    int tmp = lanesBottom();
    if( myRect.height()>tmp ) {        
        p.fillRect( QRect( 0, tmp, myRect.width(), myRect.height()-tmp+1 ), QColor( 0, 0, 0 ) );
    }
    // Time-range selection (grey band in the ruler + vertical edges over all
    // tracks). Drawn BEFORE the playhead so the cursor sits on top of it.
    drawRange( p, myRect );

    // After painting all that track stuff, we paint the cursor LAST so it is in
    // front of everything, including the time-range selection. A solid line
    // (SourceOver) reads cleanly over the grey selection band, where the old XOR
    // compositing would have produced a muddy colour.
    {
        const LocalPlayhead lp = localPlayhead();
        int x = getXPosOfOffset( lp.pos );
        if( x>=0 && x<myRect.width() ) {
            // Dimmed while this root is NOT being heard: the cursor rests
            // rather than moving, and a resting cursor must not look like a
            // playing one. Only ever dim in an arrangement tab — the master
            // is always sounding by definition.
            p.setPen( lp.sounding ? QColor( 30, 200, 30 )
                                  : QColor( 60, 110, 60 ) );
            p.drawLine( x, 0, x, myRect.height()-1 );
            // Remember where the line really landed so the next move erases THIS
            // column (globalLocatorMoved), not a drifted position.
            lastPaintedCursorX_ = x;
        } else {
            lastPaintedCursorX_ = -1;   // off-screen: nothing to erase later
        }
    }
}

/**
 * We query the given file name as sample from the project.
 * It then acts as factory.
 */
void SStdMixerView::ctInsertSample()
{
    STrack *oldTrack = qContent_->getLastClickTrack();
    if( !oldTrack ) {
        return;
    }
    // OK, we have the track. Insert the sample here.
    QFileDialog dialog(this, "Insert sample",
                       SSettings::instance().lastDir( "sample", QDir::currentPath() ),
                       "Audio and MIDI (*.wav *.mp3 *.flac *.aiff *.aif *.ogg "
                       "*.opus *.mid *.midi);;"
                       "Audio files (*.wav *.mp3 *.flac *.aiff *.aif *.ogg *.opus);;"
                       "MIDI files (*.mid *.midi);;"
                       "WAV (*.wav);;MP3 (*.mp3);;All files (*)");
    dialog.setFileMode(QFileDialog::ExistingFile);
#ifndef Q_OS_MACOS
    // macOS gets the native Finder picker (which honors the audio filter above,
    // MP3 included); other platforms keep Qt's own dialog as elsewhere in the app.
    dialog.setOptions(QFileDialog::DontUseNativeDialog);
#endif
    QString s;
    if (dialog.exec() == QDialog::Accepted) {
        s = dialog.selectedFiles().isEmpty() ? QString() : dialog.selectedFiles().at(0);
    }
    if( s.isNull() ) {
        qWarning( "Nothing selected in file requester.\n" );
        return;
    }
    SSettings::instance().setLastDir( "sample", QFileInfo( s ).absolutePath() );
//    qWarning( "User selected \"%s\" in file requester.\n", (const char *) s );
    SLink *lk = SApplication::app().getCurrentProject()->linkToFile( s );
    if( !lk ) {
        qWarning() << QString("Unable to open file \"%1\".\n").arg(s);
        return;
    }
    // The cut makes its own content link; the temporary from linkToFile() is
    // ours to delete — AFTER the cut exists, so the wave's refcount never
    // touches zero in between.
    SCut *soCut = new SCut( SApplication::app().getCurrentProject(),
                            lk->getSObject() );
    delete lk;
    SLink *cutLink = new SLink( *soCut, NULL );
    cutLink->setStartTime( qContent_->getLastClickOffset() );
    cutLink->setParent(oldTrack); // was oldTrack->insertChild( cutLink );
    // FIXME: Only update the track.
    qContent_->update();
}

void SStdMixerView::ctAddLink()
{
    SLink *oldLink = qContent_->getLastClickSLink();
    if( !oldLink ) {
        qWarning( "ctAddLink called without object.\n" );
        return;
    }
    // A TAKE COLUMN IS DEEP-COPIED, never linked (proposal 42 M1). "Add link"
    // means SHARE, and two placements of one `STakeStack` share its
    // `activeTake_` -- so comping either would comp both, which is
    // indistinguishable from comping not working. Everything else keeps the
    // sharing semantics the menu item names.
    SObject *shared = &oldLink->getSObject();
    if( STakeStack *column = dynamic_cast<STakeStack *>( shared ) ) {
        if( STakeStack *dup = stakes::cloneColumn(
                SApplication::app().getCurrentProject(), *column ) )
            shared = dup;
    }
    SLink *newLink = new SLink( *shared, NULL );
    newLink->setStartTime( oldLink->getStartTime()+oldLink->getSObject().getDuration() );
    STrack *oldTrack = qContent_->getLastClickTrack();
    if( !oldTrack ) {
        return;
    }
    newLink->setParent(oldTrack); // was: oldTrack->insertChild( newLink );
    qContent_->update();
}

void SStdMixerView::ctRemoveSample()
{
    // Delete/Backspace is this action's shortcut, and a shortcut outranks a
    // keyPressEvent — so a marquee selection of automation points is deleted
    // here, before the clip it would otherwise have removed (P6).
    if( autoUi_ && autoUi_->deleteSelection() ) return;

    STrack *oldTrack = qContent_->getLastClickTrack();
    SLink *oldLink = qContent_->getLastClickSLink();
    if( !oldTrack || !oldLink ) {
        qWarning( "ctRemoveSample called without object.\n" );
        return;
    }

    SProject *project = SApplication::app().getCurrentProject();
    if( !project ) return;

    SObject *root = splacements::rootContainer( project );
    if( !root ) return;

    // The lane as an index-path from the root mixer, so a track nested inside a
    // folder resolves too. This used to be mixer->indexOfChildObject(), which
    // only sees TOP-LEVEL children: on a grouped track it returned -1 and this
    // slot returned right here, so Delete on such a clip did nothing at all —
    // no action, no message, no undo entry.
    //
    // pathOf() returns {} for "the root itself" as well as "not found", but
    // oldTrack is an STrack and can never BE the root, so empty means not found.
    const QList<int> trackPath = strackpath::pathOf( root, oldTrack );
    if( trackPath.isEmpty() ) return;

    // Get the clip index within the track
    int clipIdx = oldTrack->indexOfChild(oldLink);
    if( clipIdx < 0 ) return;

    // The clip's source file. Passing "" here (on the theory that the inverse
    // could rebuild from track/clip index and time alone) is what made undo pop
    // "Unable to load file." and then do nothing: the inverse asked
    // linkToFile("") for a file that cannot exist. SRemoveSampleAction re-reads
    // this off the clip anyway, but a correct value keeps the SERIALIZED action
    // meaningful too.
    QString filePath;
    if( SCut *cut = dynamic_cast<SCut*>( &oldLink->getSObject() ) ) {
        if( SExternFile *xf = dynamic_cast<SExternFile*>( &cut->getContent() ) )
            filePath = xf->getFileName();
    }

    offset_t timePos = oldLink->getStartTime();

    // A placement of a registered asset BODY is its own kind of removal: the
    // clip IS the asset (SPlaceAssetAction links the body itself), so undo must
    // RE-PLACE it rather than rebuild a lookalike, or the restored clip would no
    // longer be the asset and edits to it would stop tracking. That inverse is
    // SRemoveAssetPlacementAction -> SPlaceAssetAction.
    //
    // Copies of an asset (duplicated, re-pitched) are NOT registered and fall
    // through to SRemoveSampleAction, which rebuilds the cut over the same
    // container (SRestoreContainerClipAction).
    const QString assetName = project->assetNameOf( &oldLink->getSObject() );

    qContent_->resetLastClickSLink();

    if( !assetName.isEmpty() ) {
        stimeline::submitActive(
            new SRemoveAssetPlacementAction( assetName, trackPath, clipIdx, timePos )
        );
        return;
    }

    // Submit the removal action (proper undo/redo support)
    stimeline::submitActive(
        new SRemoveSampleAction(trackPath, clipIdx, filePath, timePos)
    );
}

void SStdMixerView::ctDeleteSample()
{
}

// Split every clip in the CURRENT SELECTION whose extent strictly contains
// the split position (start < splitTime < end) — not the last-clicked object.
// When nothing is selected, fall back to the last-clicked clip (mirrors
// nudgeClipPitch's fallback, and keeps a plain single-clip click+S working).
// The split position itself is unchanged: the global locator, exactly as
// before.
void SStdMixerView::ctSplitSample()
{
    SApplication &app = SApplication::app();
    SProject *project = app.getCurrentProject();
    if( !project ) return;

    // THIS view's playhead, not the transport's: in an arrangement tab the
    // global locator is a master frame and would split at an unrelated moment
    // (proposal 09 §15). In the master the two are the same number.
    offset_t splitTime = qContent_ ? qContent_->localLocatorPos()
                                   : app.getGlobalLocatorPos();

    QList<QList<int>> paths = app.getCurrentSelectionPaths();
    if( paths.isEmpty() ) {
        STrack *track = qContent_->getLastClickTrack();
        SLink  *link  = qContent_->getLastClickSLink();
        if( track && link ) {
            QList<int> p = strackpath::pathOf( model_, track );
            p.append( track->indexOfChild( link ) );
            paths.append( p );
        }
    }
    if( paths.isEmpty() ) {
        qWarning( "ctSplitSample called without object.\n" );
        return;
    }

    // Each target clip gets its own extent check BEFORE the action runs:
    // SCompositeAction rolls back the whole macro if any child fails to
    // apply, so a clip the split position does not strictly fall inside must
    // never be handed to SSplitClipAction in the first place. The 1-frame
    // margin on each side matches SSplitClipAction::apply's own guard against
    // a zero/negligible-length remainder, so a clip that passes here is one
    // the action will actually accept.
    SObject *mixer = splacements::rootContainer( project );
    SCompositeAction *composite = new SCompositeAction;
    int nTargets = 0;
    for( const QList<int> &p : paths ) {
        SLink *lk = splacements::placementAt( mixer, p );
        if( !lk || lk->getSObject().isLane() ) continue;
        offset_t startTime = lk->getStartTime();
        length_t fullDur = lk->getSObject().getDurationBlocking();
        offset_t inObjOffset = splitTime - startTime;
        if( inObjOffset <= 1 || inObjOffset >= (offset_t)fullDur - 1 ) continue;
        composite->append( new SSplitClipAction( p, splitTime ) );
        ++nTargets;
    }
    if( nTargets == 0 ) {
        delete composite;
        return;
    }
    qContent_->resetLastClickSLink();   // any split link may be replaced
    stimeline::submitActive( composite );
    qContent_->update();
}

// --- proposal 41 M2: pack / unpack from the arranger ------------------------
//
// PACK ACTS ON THE SELECTION, not on the right-clicked clip. Packing is a
// group operation and a single clip has nothing to group with, so there is no
// last-clicked fallback here (unlike ctSplitSample / nudgeClipPitch, where a
// one-clip target is the normal case).
//
// A selection that crossed several lanes is NOT refused: `pack-selection`
// partitions it and mints one fragment per lane holding two or more selected
// clips, leaving every lane that holds exactly one alone. `pack-clips` itself
// stays single-lane by contract (D8, AC2.5), which is what
// fragment_pack_multilane_refused.qxa gates -- the partition lives one level
// above it rather than as a mode flag inside it.
//
// The names are GENERATED ("<lane name> N", pack-clips' own fallback) and
// there is deliberately no prompt: one dialog cannot name N lanes' fragments,
// and unlike "Create asset from range" -- whose name becomes a TAB LABEL --
// a fragment's name shows only on its tag chip and is renameable afterwards.
void SStdMixerView::ctPackClips()
{
    SApplication &app = SApplication::app();
    if( !app.getCurrentProject() ) return;

    auto showStatus = [this]( const QString &msg ) {
        if( QMainWindow *mw = qobject_cast<QMainWindow*>( window() ) )
            mw->statusBar()->showMessage( msg, 4000 );
    };

    const QList<QList<int>> paths =
        app.getCurrentSelectionPathsFor( rootName() );
    const int nLanes = spackselection::packableLaneCount( paths );
    if( nLanes == 0 ) {
        showStatus( "Select two or more clips on the same lane to pack them" );
        return;
    }

    // The packed clips are REPARENTED into the fragments, not deleted, so the
    // selection would survive as a set of links now living INSIDE them --
    // and Delete over that selection would cascade into every placement
    // (objects/fragment/CONTRACT.md inv. 6), which is not what a user who
    // just packed and pressed Delete means. Cleared as plain UI state, like
    // resetLastClickSLink() below; undo restores the clips, not the selection.
    app.clearSelection( rootName() );
    qContent_->resetLastClickSLink();

    stimeline::submitActive( new SPackSelectionAction( paths ) );
    showStatus( nLanes > 1
                    ? QString( "Packed %1 lanes into fragments" ).arg( nLanes )
                    : QString( "Packed the selection into a fragment" ) );
    qContent_->update();
}

// Unpack the right-clicked fragment placement. Both refusals the action makes
// are already reflected in the menu item's enabled state (see ctGlobalShow);
// they are re-checked here because a slot is reachable however it was
// triggered, and a status line beats a log line nobody has open.
void SStdMixerView::ctUnpackClips()
{
    SApplication &app = SApplication::app();
    if( !app.getCurrentProject() || !model_ ) return;

    auto showStatus = [this]( const QString &msg ) {
        if( QMainWindow *mw = qobject_cast<QMainWindow*>( window() ) )
            mw->statusBar()->showMessage( msg, 4000 );
    };

    STrack *track = qContent_->getLastClickTrack();
    SLink  *link  = qContent_->getLastClickSLink();
    if( !track || !link ) {
        showStatus( "Right-click a packed fragment to unpack it" );
        return;
    }
    SCut *cut = dynamic_cast<SCut *>( &link->getSObject() );
    if( !cut || !cut->getContent().isLaneFragment() ) {
        showStatus( "That clip is not a packed fragment" );
        return;
    }
    if( cut->refCount() != 2 ) {
        showStatus( QString( "\"%1\" is placed %2 times; unpacking it would "
                             "empty it out from under the other placements" )
                        .arg( cut->getSName() ).arg( cut->refCount() - 1 ) );
        return;
    }

    QList<int> p = strackpath::pathOf( model_, track );
    p.append( track->indexOfChild( link ) );

    // The placement link is DELETED by the action; a stale lastClickSLink_
    // would dangle (same reason ctSplitSample resets it). The SELECTION needs
    // no clearing -- SApplication watches each selected link's destroyed()
    // and drops it.
    qContent_->resetLastClickSLink();
    stimeline::submitActive( new SUnpackClipsAction( p ) );
    qContent_->update();
}

// Transpose the target clips by `cents`. Pitch lives in the grain stage, so
// this changes only how the clip SOUNDS — its window, its length and every
// position mapping stay exactly where they were.
void SStdMixerView::nudgeClipPitch( double cents )
{
    SApplication &app = SApplication::app();
    SProject *project = app.getCurrentProject();
    if( !project ) return;

    auto showStatus = [this]( const QString &msg ) {
        if( QMainWindow *mw = qobject_cast<QMainWindow*>( window() ) )
            mw->statusBar()->showMessage( msg, 4000 );
    };

    // Targets: the selection, or — when nothing is selected — the clip the
    // user last clicked (what S / Delete already act on).
    QList<QList<int>> paths = app.getCurrentSelectionPaths();
    if( paths.isEmpty() ) {
        STrack *track = qContent_->getLastClickTrack();
        SLink  *link  = qContent_->getLastClickSLink();
        if( track && link ) {
            QList<int> p = strackpath::pathOf( model_, track );
            p.append( track->indexOfChild( link ) );
            paths.append( p );
        }
    }
    if( paths.isEmpty() ) {
        showStatus( "Select a clip first" );
        return;
    }

    // Each clip gets its own ABSOLUTE target, so a nudge over a mixed
    // selection preserves the intervals between the clips.
    SObject *mixer = splacements::rootContainer( project );
    SCompositeAction *composite = new SCompositeAction;
    double lastCents = 0.0;
    int nTargets = 0;
    for( const QList<int> &p : paths ) {
        SLink *lk = splacements::placementAt( mixer, p );
        if( !lk ) continue;
        SCut *cut = dynamic_cast<SCut*>( &lk->getSObject() );
        if( !cut ) {
            // A take stack transposes its ACTIVE take (pitch is per-take).
            if( STakeStack *stack = dynamic_cast<STakeStack*>( &lk->getSObject() ) )
                cut = dynamic_cast<SCut*>( stack->activeTakeObject() );
        }
        if( !cut ) continue;
        double target = SCut::clampPitchCents( cut->getPitchCents() + cents );
        // Already at the limit: nudging further would push an undo step that
        // changes nothing.
        if( target == cut->getPitchCents() ) continue;
        lastCents = target;
        composite->append( new SSetPitchAction( p, target ) );
        ++nTargets;
    }
    if( nTargets == 0 ) {
        delete composite;
        showStatus( QString( "No clip to transpose (limit is +/-%1 cents)" )
                        .arg( SCut::PITCH_CENTS_LIMIT ) );
        return;
    }

    stimeline::submitActive( composite );

    QString msg = ( nTargets == 1 )
        ? QString( "Clip pitch: %1%2 cents (%3%4 st)" )
              .arg( lastCents > 0 ? "+" : "" ).arg( lastCents, 0, 'g', 6 )
              .arg( lastCents > 0 ? "+" : "" ).arg( lastCents / 100.0, 0, 'g', 3 )
        : QString( "Transposed %1 clips by %2%3 cents" )
              .arg( nTargets ).arg( cents > 0 ? "+" : "" ).arg( cents );
    showStatus( msg );
    qContent_->update();
}

void SMVActualView::ctToggleTakeLanes()
{
    if( !lastClickTrack_ ) return;
    // The clicked lane decides the direction; the rest of the selection is
    // driven TO that state rather than each toggled on its own, so a mixed
    // selection ends up uniform instead of inverted.
    const bool want = !smv_.isTrackTakesExpanded( lastClickTrack_ );
    for( STrack *t : smv_.selectionTargets( lastClickTrack_ ) ) {
        if( smv_.isTrackTakesExpanded( t ) != want )
            smv_.toggleTrackTakesExpanded( t );
    }
}

// Clip context menu: open the clip properties panel (proposal 31). The panel
// edits the SELECTION, not the clicked clip, so a right-click on a clip that is
// not part of the current selection selects it first — otherwise the menu would
// appear to act on one clip while the panel showed another.
void SMVActualView::ctShowClipProperties()
{
    if( lastClickSLink_
        && !SApplication::app().isSLinkSelected( lastClickSLink_ ) ) {
        SApplication::app().submitSetSelectionAction( lastClickSLink_ );
    }
    SMainWindow *mw = dynamic_cast<SMainWindow*>( QApplication::activeWindow() );
    if( mw ) mw->showClipProperties();
}

void SMVActualView::ctGlobalShow()
{
    qGlobalPopup_->clear();
    if( lastClickSLink_ ) {
        // Proposal 31: the per-clip PROPERTIES (pitch, loop, formants) used to
        // live here as individual items. They now live in the clip properties
        // panel, which edits the whole selection rather than just the clicked
        // clip. What stays here is structural: split and link.
        // The pitch actions themselves are untouched — they are registered on
        // the view with addAction() (see the ctor), so +/- keep transposing.
        qGlobalPopup_->addAction( "Clip &properties...", this,
                                  SLOT( ctShowClipProperties() ) );
        qGlobalPopup_->addSeparator();
        qGlobalPopup_->addAction( smv_.actSplit_ );
        qGlobalPopup_->addAction( "Add &link", &smv_, SLOT( ctAddLink() ) );

        // --- proposal 41 M2: the arranger's surface for pack / unpack -----
        // Pack acts on the SELECTION (packing is a group operation), unpack
        // on the CLICKED clip (there is exactly one fragment under the
        // pointer). Both items announce what they would do rather than
        // failing after the fact, the way "Create asset from range" already
        // does when there is no range.
        {
            const int nLanes = spackselection::packableLaneCount(
                SApplication::app().getCurrentSelectionPathsFor(
                    smv_.rootName() ) );
            QAction *aPack = qGlobalPopup_->addAction(
                nLanes > 1
                    ? QString( "Pac&k clips into %1 fragments" ).arg( nLanes )
                    : QString( "Pac&k clips into fragment" ),
                &smv_, SLOT( ctPackClips() ) );
            if( nLanes == 0 ) {
                aPack->setEnabled( false );
                aPack->setText( "Pac&k clips into fragment  "
                                "(select two or more clips on one lane)" );
            }
        }
        // Offered only over a fragment placement -- every other clip has
        // nothing to take apart. Asked through SObject::isLaneFragment()
        // because app/timeline has no edge to app/objects/fragment.
        if( SCut *cut = dynamic_cast<SCut *>( &lastClickSLink_->getSObject() ) ) {
            if( cut->getContent().isLaneFragment() ) {
                QAction *aUnpack = qGlobalPopup_->addAction(
                    QString( "&Unpack \"%1\"" ).arg( cut->getSName() ),
                    &smv_, SLOT( ctUnpackClips() ) );
                // unpack-clips REFUSES a shared asset: it moves every child
                // out, which would leave any other placement windowing an
                // emptied fragment (D2's sharing invariant). refCount() is
                // the registry pin plus one per placement, so 2 means this is
                // the only one -- the same number the action itself checks.
                if( cut->refCount() != 2 ) {
                    aUnpack->setEnabled( false );
                    aUnpack->setText(
                        aUnpack->text()
                        + QString( "  (placed %1 times \u2014 unpacking would "
                                   "break sharing)" ).arg( cut->refCount() - 1 ) );
                }
            }
        }
        qGlobalPopup_->addSeparator();
    }
    if( lastClickTrack_ ) {
        qGlobalPopup_->addAction( smv_.actInsertSample_ );
        qGlobalPopup_->addAction( smv_.actRemoveSample_ );
        qGlobalPopup_->addAction( "Delete sample", &smv_, SLOT( ctDeleteSample() ) );
        qGlobalPopup_->addSeparator();
    }
    // "New track" from THIS menu goes below the lane the menu was opened on —
    // the menu is aimed at a position, and every other track item in it acts
    // there. So it is built per show against lastClickTrack_ rather than
    // reusing smv_.actNewTrack_, whose Ctrl+T path has no click to aim at and
    // follows the selection instead. Built fresh each time like the other
    // per-click items above; qGlobalPopup_->clear() owns and deletes it.
    {
        QAction *aNew = qGlobalPopup_->addAction( smv_.actNewTrack_->text() );
        // Display only. WidgetShortcut binds it to the menu, which has focus
        // only while it is up, so it can never go ambiguous with the view's
        // window-level Ctrl+T.
        aNew->setShortcut( smv_.actNewTrack_->shortcut() );
        aNew->setShortcutContext( Qt::WidgetShortcut );
        // By QPointer: the lane could in principle be gone by the time the
        // action fires, and a dead reference just means "append".
        const QPointer<STrack> ref( lastClickTrack_ );
        QObject::connect( aNew, &QAction::triggered, this,
                          [this, ref]() { smv_.addTrackBelow_( ref.data() ); } );
    }
    if( lastClickTrack_ ) {
        // How many tracks the track items below will actually act on. The menu
        // says so out loud: an item that silently hits four lanes when the
        // pointer is on one is the whole risk of a multi-selection.
        const int nSel = smv_.selectionTargets( lastClickTrack_ ).size();
        const QString sfx = ( nSel > 1 ) ? QString( " (%1 tracks)" ).arg( nSel )
                                         : QString();
        qGlobalPopup_->addAction( "Remove track" + sfx, &smv_, SLOT( ctRemoveTrack() ) );
        qGlobalPopup_->addSeparator();
        qGlobalPopup_->addAction(
            smv_.isTrackTakesExpanded( lastClickTrack_ )
                ? "Hide take lanes" : "Show take lanes",
            this, SLOT( ctToggleTakeLanes() ) );
        // Per-track lane height, as a factor of the base (zoomed) height, so
        // one track can be tall for detailed editing while its neighbours stay
        // small. Vertical zoom keeps scaling all of them together.
        //
        // Built once and kept: qGlobalPopup_->clear() drops the submenu's
        // action but not the submenu, and the handlers read lastClickTrack_ at
        // trigger time, so there is nothing to rewire per right-click.
        if( !qLaneHeightMenu_ ) {
            qLaneHeightMenu_ = new QMenu( "Lane &height", qGlobalPopup_ );
            const struct { const char *label; double scale; } presets[] = {
                { "&Small",  0.5 }, { "&Normal", 1.0 },
                { "&Large",  1.5 }, { "&Extra large", 2.5 },
            };
            for( const auto &p : presets ) {
                QAction *a = qLaneHeightMenu_->addAction( p.label );
                a->setCheckable( true );
                a->setData( p.scale );
                QObject::connect( a, &QAction::triggered, this,
                                  [this, a]() {
                                      for( STrack *t : smv_.selectionTargets( lastClickTrack_ ) )
                                          smv_.setTrackHeightScale(
                                              t, a->data().toDouble() );
                                  } );
            }
        }
        {
            const double cur = smv_.trackHeightScale( lastClickTrack_ );
            for( QAction *a : qLaneHeightMenu_->actions() )
                a->setChecked( qFuzzyCompare( cur, a->data().toDouble() ) );
        }
        qGlobalPopup_->addMenu( qLaneHeightMenu_ );
        // The automation picker (proposal 37 P6). Rebuilt per right-click
        // because a track's plugin parameters change under it.
        smv_.automationUi().buildPickerMenu( qGlobalPopup_, lastClickTrack_ );
        qGlobalPopup_->addSeparator();
        qGlobalPopup_->addAction( "Indent track (nest under above)" + sfx,
                                  &smv_, SLOT( ctIndentTrack() ) );
        qGlobalPopup_->addAction( "Outdent track" + sfx, &smv_, SLOT( ctOutdentTrack() ) );
        // One folder for the whole block, not one each.
        qGlobalPopup_->addAction(
            nSel > 1 ? QString( "Group %1 tracks" ).arg( nSel )
                     : QString( "Group track" ),
            &smv_, SLOT( ctGroupTrack() ) );
        if( smv_.rowIndexOfTrack( lastClickTrack_ )>=0
            && smv_.rowAt( smv_.rowIndexOfTrack( lastClickTrack_ ) )->hasChildren ) {
            qGlobalPopup_->addAction( "Ungroup track", &smv_, SLOT( ctUngroupTrack() ) );
        }
        qGlobalPopup_->addSeparator();
        // Make a live asset from this track over the current ruler range. Needs
        // a range; disabled (with a hint) when none is selected.
        QAction *aAsset = qGlobalPopup_->addAction(
            "Create &asset from range", this, SLOT( ctCreateAssetFromTrack() ) );
        aAsset->setEnabled( hasRange() );
        if( !hasRange() )
            aAsset->setText( "Create &asset from range  (select a range first)" );
    }
}

// --- multi-track selection --------------------------------------------------
//
// The set lives on SStdMixer; what lives here is the GESTURE side of it — how a
// click becomes a selection, which tracks an operation then acts on, and in what
// order. See the block comment on these declarations in the header for the one
// rule that governs every track operation below.

QList<STrack *> SStdMixerView::tracksBetween( STrack *a, STrack *b ) const
{
    QList<STrack *> out;
    const int ra = rowIndexOfTrack( a );
    const int rb = rowIndexOfTrack( b );
    if( ra < 0 || rb < 0 ) {
        // One end is not on screen (a collapsed folder's child, say). Degrade
        // to the ends themselves rather than selecting nothing.
        if( a ) out.append( a );
        if( b && b != a ) out.append( b );
        return out;
    }
    const int lo = qMin( ra, rb ), hi = qMax( ra, rb );
    for( int i = lo; i <= hi; ++i ) {
        const STrackRow *row = rowAt( i );
        // Sub-lanes (take lanes) belong to the track lane above them and are
        // not separately selectable.
        if( !row || row->isSubLane() || !row->track ) continue;
        if( !out.contains( row->track ) ) out.append( row->track );
    }
    return out;
}

QList<STrack *> SStdMixerView::orderByLane( const QList<STrack *> &in ) const
{
    QList<STrack *> out = in;
    std::stable_sort( out.begin(), out.end(),
                      [this]( STrack *l, STrack *r ) {
                          const int li = rowIndexOfTrack( l );
                          const int ri = rowIndexOfTrack( r );
                          // Tracks with no visible lane sort last, stably.
                          if( li < 0 ) return false;
                          if( ri < 0 ) return true;
                          return li < ri;
                      } );
    return out;
}

QList<STrack *> SStdMixerView::pruneNestedTargets( const QList<STrack *> &in )
{
    QList<STrack *> out;
    for( STrack *t : in ) {
        bool covered = false;
        for( STrack *other : in ) {
            if( other == t ) continue;
            // isSelfOrDescendant( candidate, ancestor ): is t below other?
            if( strackpath::isSelfOrDescendant( t, other ) ) { covered = true; break; }
        }
        if( !covered ) out.append( t );
    }
    return out;
}

QList<STrack *> SStdMixerView::selectionTargets( STrack *clicked ) const
{
    QList<STrack *> out;
    if( !clicked ) return out;
    // THE rule: only a gesture aimed INTO the selection broadcasts. Aiming at a
    // lane outside it acts on that lane alone, so an operation can never reach
    // a track the user is not pointing at.
    if( model_ && model_->isTrackSelected( clicked )
        && model_->nSelectedTracks() > 1 ) {
        return orderByLane( model_->getSelectedTracks() );
    }
    out.append( clicked );
    return out;
}

void SStdMixerView::applyTrackSelectionClick( STrack *t, Qt::KeyboardModifiers mods )
{
    if( !t || !model_ ) return;
    const bool ctrl  = mods.testFlag( Qt::ControlModifier );
    const bool shift = mods.testFlag( Qt::ShiftModifier );

    if( shift ) {
        STrack *anchor = selectionAnchor_.data();
        if( !anchor || rowIndexOfTrack( anchor ) < 0 ) anchor = model_->getSelectedTrack();
        if( !anchor ) anchor = t;
        QList<STrack *> range = tracksBetween( anchor, t );
        if( ctrl ) {
            // Ctrl+Shift: add the range to what is already selected.
            QList<STrack *> merged = model_->getSelectedTracks();
            for( STrack *r : range ) if( !merged.contains( r ) ) merged.append( r );
            model_->setSelectedTracks( merged, t );
        } else {
            model_->setSelectedTracks( range, t );
        }
        // The anchor deliberately does NOT move, so successive Shift-clicks
        // pivot around the same lane instead of walking away from it.
        return;
    }

    selectionAnchor_ = t;
    if( ctrl ) model_->toggleTrackSelection( t );
    else       model_->setSelectedTrack( t );
}

void SStdMixerView::showTrackContextMenu( STrack *t, const QPoint &globalPos )
{
    if( !t || !qContent_ ) return;
    // A right-click on a lane OUTSIDE the selection selects it first, so the
    // menu can never appear to act on one track while operating on others —
    // the same rule the clip properties menu follows.
    if( model_ && !model_->isTrackSelected( t ) ) {
        applyTrackSelectionClick( t, Qt::NoModifier );
    }
    qContent_->popupTrackMenu( t, globalPos );
}

/**
 * Create a new, empty track, insert a new link to it into this arrangement.
 */
void SStdMixerView::ctAddTrack()
{
    // Route through the action: undoable, and it rewires the speaker so the new
    // track is audible (the old direct insertTrack did neither).
    //
    // The new track lands BELOW a reference lane, not at the bottom of the
    // arrangement; on a long arrangement the append also put it off screen.
    //
    // THIS entry point is the Ctrl+T shortcut, which names no click, so the
    // reference is the selection. The context menu does NOT come through here:
    // it is aimed at a position and builds its own item against the lane it was
    // opened on (see SMVActualView::ctGlobalShow).
    addTrackBelow_( newTrackReference_() );
}

STrack *SStdMixerView::newTrackReference_() const
{
    if( !model_ ) return nullptr;
    // The LAST selected lane by position, so Ctrl+T after selecting a block
    // lands under the block rather than under whichever member was clicked.
    // add-track SELECTS the track it just made, so repeated Ctrl+T walks
    // downwards; keyed off a click that may be many gestures old, every new
    // lane would pile into the same slot instead, in reverse order.
    const QList<STrack *> sel = orderByLane( model_->getSelectedTracks() );
    if( !sel.isEmpty() ) return sel.last();
    // Nothing selected: fall back to the last lane the user aimed at. With
    // neither there is nothing to be below, and addTrackBelow_ appends.
    if( qContent_ ) return qContent_->getLastClickTrack();
    return nullptr;
}

void SStdMixerView::addTrackBelow_( STrack *ref )
{
    if( !model_ ) return;

    const QList<int> refPath = ref ? strackpath::pathOf( model_, ref )
                                   : QList<int>();
    if( refPath.isEmpty() ) {
        // No reference, or one that is not in the tree: append, as before.
        stimeline::submitActive( new SAddTrackAction( -1 ) );
        return;
    }

    QList<int> parentPath = refPath;
    const int slot = parentPath.takeLast();   // ref's slot in its container

    if( parentPath.isEmpty() ) {
        // Top level. add-track takes a TRACK index and a path step is a CHILD
        // index, but the root mixer holds nothing except tracks, so the two
        // coincide there — which is what lets this be one action instead of a
        // macro. (ctGroupTrack() leans on the same identity.)
        stimeline::submitActive( new SAddTrackAction( slot + 1 ) );
        return;
    }

    // NESTED: add-track can only append at the MIXER's top level, so the track
    // is born there and moved into the reference's container — two actions
    // wrapped in one undo step, the ctGroupTrack()/ctAddTrackBelowLast()
    // pattern.
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    if( stack ) stack->beginMacro( "Add track" );
    stimeline::submitActive( new SAddTrackAction( -1 ) );
    // submitAction drains synchronously (Phase 1), so the new track is now the
    // last top-level one. An APPEND at the top level shifts no existing index,
    // so parentPath and slot are still the ones we measured.
    const int newIdx = model_->getNTracks() - 1;
    stimeline::submitActive( new SReparentTrackAction(
        QList<int>{ newIdx }, parentPath, slot + 1 ) );
    if( stack ) stack->endMacro();
}

/**
 * Add a new track whose parent is the same container as the last visible lane's
 * track (the "track above" the blank space the user double-clicked or dropped
 * media onto). Always goes through the undoable SAction system.
 *
 * When that parent is the root mixer the whole gesture is a single
 * SAddTrackAction. When it is a folder track we follow the ctGroupTrack()
 * pattern: append a top-level track, then reparent it under the folder — two
 * SActions wrapped in one undo macro so a single undo reverses the gesture.
 */
STrack *SStdMixerView::ctAddTrackBelowLast()
{
    if( !model_ ) return nullptr;

    // "Track above" = the last lane in the flattened tree; its container is the
    // parent the new track should share. No lanes yet -> append to the mixer.
    const STrackRow *last = rowAt( rowCount() - 1 );
    SObject *parent = last ? last->parent : (SObject*)model_;
    STrack *parentTrack = dynamic_cast<STrack*>( parent );

    if( !parentTrack ) {
        // Parent is the root mixer: a plain append is undoable on its own.
        stimeline::submitActive( new SAddTrackAction( -1 ) );
        // submitAction drains synchronously (Phase 1), so the new track is
        // now the last top-level child.
        return dynamic_cast<STrack*>( strackpath::resolveByPath(
            model_, QList<int>{ model_->getNTracks() - 1 } ) );
    }

    // Nested parent: create at the top level, then move under the folder.
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    if( stack ) stack->beginMacro( "Add track" );
    stimeline::submitActive( new SAddTrackAction( -1 ) );
    // submitAction drains synchronously (Phase 1), so the new track is now the
    // last top-level track; reparent it under the target folder.
    int newIdx = model_->getNTracks() - 1;
    STrack *newTrack = dynamic_cast<STrack*>(
        strackpath::resolveByPath( model_, QList<int>{ newIdx } ) );
    stimeline::submitActive( new SReparentTrackAction(
        QList<int>{ newIdx },
        strackpath::pathOf( model_, parentTrack ), -1 ) );
    if( stack ) stack->endMacro();
    // The reparent moves the SLink, not the STrack — newTrack (pinned across
    // the move by SReparentTrackAction's addRef) is still the right pointer.
    return newTrack;
}

// Catch double-clicks in the blank area of the track-control column (below the
// last track head) and turn them into a new-track gesture — and keep the heads
// glued to the lanes when the column is resized.
bool SStdMixerView::eventFilter( QObject *watched, QEvent *event )
{
    if( watched == qTrackControlBox_ && event->type() == QEvent::Resize ) {
        // The layout just (re)sized the viewport: re-place the heads inside it.
        layoutControlColumn();
        return false;      // let the widget see its own resize too
    }
    if( ( watched == qTrackControlBoxHolder_ || watched == qTrackControlBox_ )
        && event->type() == QEvent::Wheel ) {
        // Blank area below the last head (a head forwards its own wheel events,
        // see SSMVMixerControl::wheelEvent): same gestures as over the canvas.
        if( wheelFromHead( static_cast<QWheelEvent*>( event ) ) )
            return true;
    }
    if( ( watched == qTrackControlBoxHolder_ || watched == qTrackControlBox_ )
        && event->type() == QEvent::MouseButtonDblClick ) {
        QMouseEvent *me = static_cast<QMouseEvent*>( event );
        // Only the blank area below the last head adds a track. A double click
        // that landed on a head propagates up to here (heads do not consume
        // it), and must not spawn a track.
        if( me->button() == Qt::LeftButton
            && !rowAt( rowAtControlY( (int) me->position().y() ) ) ) {
            ctAddTrackBelowLast();
            return true;   // consumed
        }
    }
    return QWidget::eventFilter( watched, event );
}

// The track heads have no time axis of their own, so a wheel over them is
// simply the canvas' gesture: -1 as the anchor keeps the left edge on a
// horizontal zoom (there is no time under the pointer to hold still).
bool SStdMixerView::wheelFromHead( QWheelEvent *ev )
{
    return qContent_ ? qContent_->applyWheel( ev, -1 ) : false;
}

/**
 * Remove the tracks the gesture targets (the whole selection when it was aimed
 * into it). Each removal also removes that track's children.
 */
void SStdMixerView::ctRemoveTrack()
{
    STrack *clicked = qContent_->getLastClickTrack();
    if( !clicked || !model_ ) return;
    // Outermost tracks only: removing a folder takes its subtree with it, so a
    // selected child inside a selected folder must not be removed twice (its
    // path would resolve to a different track by then).
    const QList<STrack*> targets =
        pruneNestedTargets( selectionTargets( clicked ) );
    if( targets.isEmpty() ) return;

    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Remove tracks" ) );
    // Bottom-up: every removal shifts the indices of the lanes below it, and
    // the paths are re-derived per step anyway (submitAction drains
    // synchronously), but going upwards keeps each re-derivation cheap and the
    // undo order the mirror of the redo order.
    for( int i = targets.size()-1; i >= 0; --i ) {
        // Index-PATH from the root mixer, so a track inside a folder can be
        // removed too. This used to be model_->indexOfChildObject(), which sees
        // the mixer's DIRECT children only: on a nested track it returned -1 and
        // this slot returned right here, so "Remove track" did nothing at all —
        // no action, no message, no undo entry.
        const QList<int> path = strackpath::pathOf( model_, targets.at( i ) );
        if( path.isEmpty() ) continue;      // not in this project's tree
        // Through the action so it is undoable (the track + subtree is restorable).
        stimeline::submitActive( new SRemoveTrackAction( path ) );
    }
    if( macro ) stack->endMacro();
    // Nothing that was removed may stay selected.
    if( model_ ) {
        QList<STrack*> keep;
        for( STrack *s : model_->getSelectedTracks() )
            if( !strackpath::pathOf( model_, s ).isEmpty() ) keep.append( s );
        model_->setSelectedTracks( keep );
    }
}

// --- grouping (proposal 05 §1.2) ----------------------------------------

// One track's worth of "indent": nest it under its nearest preceding sibling
// that is not itself on the move. `alsoMoving` matters only for the very first
// step of a multi-track indent — later steps see a tree the earlier ones
// already changed, because submitAction drains synchronously and the view
// rebuilds its rows on the tree-changed signal.
bool SStdMixerView::indentOne_( STrack *t, const QList<STrack*> &alsoMoving )
{
    if( !t || !model_ ) return false;
    int ri = rowIndexOfTrack( t );
    if( ri<0 ) return false;
    const STrackRow *row = rowAt( ri );
    SObject *parent = row->parent;
    int depth = row->depth;
    // Preceding sibling = the nearest earlier row sharing this parent.
    STrack *prevSibling = NULL;
    for( int i=ri-1; i>=0; --i ) {
        const STrackRow *r = rowAt( i );
        if( r->depth < depth ) break;          // left this sibling group
        if( r->parent != parent ) continue;
        if( alsoMoving.contains( r->track ) ) continue;   // it is moving too
        prevSibling = r->track;
        break;
    }
    if( !prevSibling ) return false;             // nothing to nest under
    stimeline::submitActive( new SReparentTrackAction(
        strackpath::pathOf( model_, t ),
        strackpath::pathOf( model_, prevSibling ), -1 ) );
    return true;
}

void SStdMixerView::ctIndentTrack()
{
    STrack *clicked = qContent_->getLastClickTrack();
    if( !clicked || !model_ ) return;
    const QList<STrack*> targets =
        pruneNestedTargets( selectionTargets( clicked ) );
    if( targets.isEmpty() ) return;

    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Indent tracks" ) );
    // Top down: the first target nests under the lane above the block, and each
    // later one then finds that same lane as its preceding sibling — so a
    // contiguous block lands inside one folder, in order.
    for( STrack *t : targets ) indentOne_( t, targets );
    if( macro ) stack->endMacro();
}

// One track's worth of "outdent": move it to its grandparent, just after the
// folder it is leaving.
bool SStdMixerView::outdentOne_( STrack *t )
{
    if( !t || !model_ ) return false;
    int ri = rowIndexOfTrack( t );
    if( ri<0 ) return false;
    SObject *parent = rowAt( ri )->parent;
    STrack *parentTrack = dynamic_cast<STrack*>( parent );
    if( !parentTrack ) return false;             // already top-level
    int pri = rowIndexOfTrack( parentTrack );
    SObject *grand = (pri>=0) ? rowAt( pri )->parent : (SObject*)model_;
    int dstIndex = grand->indexOfChildObject( *parentTrack ) + 1;  // just after parent
    stimeline::submitActive( new SReparentTrackAction(
        strackpath::pathOf( model_, t ),
        strackpath::pathOf( model_, grand ), dstIndex ) );
    return true;
}

void SStdMixerView::ctOutdentTrack()
{
    STrack *clicked = qContent_->getLastClickTrack();
    if( !clicked || !model_ ) return;
    const QList<STrack*> targets =
        pruneNestedTargets( selectionTargets( clicked ) );
    if( targets.isEmpty() ) return;

    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Outdent tracks" ) );
    // BOTTOM UP: each track lands immediately after the folder it left, so
    // promoting the lowest one first is what keeps the block's relative order
    // (top-down would reverse it).
    for( int i = targets.size()-1; i >= 0; --i ) outdentOne_( targets.at( i ) );
    if( macro ) stack->endMacro();
}

void SStdMixerView::ctGroupTrack()
{
    STrack *clicked = qContent_->getLastClickTrack();
    if( !clicked || !model_ ) return;
    // One new folder for the whole target block — not one per track.
    const QList<STrack*> targets =
        pruneNestedTargets( selectionTargets( clicked ) );
    if( targets.isEmpty() ) return;

    STrack *first = targets.first();
    const QList<int> tPath = strackpath::pathOf( model_, first );
    if( tPath.isEmpty() ) return;
    QList<int> parentPath = tPath;
    const int ti = parentPath.takeLast();       // first target's slot in its parent

    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    if( stack ) stack->beginMacro( "Group track" );

    if( parentPath.isEmpty() ) {
        // The block is top-level: create the folder directly in its slot. (This
        // is the only case the gesture used to handle at all.)
        stimeline::submitActive( new SAddTrackAction( ti ) );
    } else {
        // NESTED. add-track can only append at the MIXER's top level, and
        // SReparentTrackAction refuses a same-container move (that is
        // SMoveTrackAction's job) — so the folder cannot be born in place and
        // cannot be slid there afterwards if it starts as a sibling. Create it
        // top-level, then move it INTO the parent at the target's slot (a real
        // cross-container reparent), which pushes the targets down by one.
        stimeline::submitActive( new SAddTrackAction( -1 ) );
        const int folderTop = model_->getNTracks() - 1;   // append landed last
        stimeline::submitActive( new SReparentTrackAction(
            QList<int>{ folderTop }, parentPath, ti ) );
    }
    // Resolve the folder BY POINTER from here on: every reparent below shifts
    // the indices its path would otherwise have been spelled with.
    QList<int> folderPath = parentPath; folderPath.append( ti );
    STrack *folder = dynamic_cast<STrack*>(
        strackpath::resolveByPath( model_, folderPath ) );
    if( folder ) {
        for( STrack *t : targets ) {
            const QList<int> src = strackpath::pathOf( model_, t );
            if( src.isEmpty() ) continue;
            stimeline::submitActive( new SReparentTrackAction(
                src, strackpath::pathOf( model_, folder ), -1 ) );
        }
    }

    if( stack ) stack->endMacro();
}

void SStdMixerView::ctUngroupTrack()
{
    STrack *clicked = qContent_->getLastClickTrack();
    if( !clicked || !model_ ) return;
    const QList<STrack*> targets =
        pruneNestedTargets( selectionTargets( clicked ) );
    if( targets.isEmpty() ) return;

    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Ungroup tracks" ) );
    for( int i = targets.size()-1; i >= 0; --i ) ungroupOne_( targets.at( i ) );
    if( macro ) stack->endMacro();
}

// Dissolve one folder track, promoting its children into the folder's own
// parent, then deleting the (now empty) folder.
void SStdMixerView::ungroupOne_( STrack *t )
{
    if( !t || !model_ ) return;
    const QList<int> tPath = strackpath::pathOf( model_, t );
    if( tPath.isEmpty() ) return;
    QList<int> parentPath = tPath;
    const int ti = parentPath.takeLast();       // the folder's slot in ITS parent

    QList<STrack*> kids;
    for( SLink *lk : t->childLinks() ) {
        if( STrack *k = dynamic_cast<STrack*>( &lk->getSObject() ) ) kids.append( k );
    }
    if( kids.isEmpty() ) return;

    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    if( stack ) stack->beginMacro( "Ungroup track" );
    // Promote each child into the folder's OWN parent — the mixer when the
    // folder is top-level, the grandparent folder when it is nested (this used
    // to hard-code the mixer, so ungrouping a nested folder would have flung its
    // children out to the top level). Fill the slots just before the folder so
    // they end up where it was, in order; each insert pushes the folder one
    // further right, which is why insertAt just increments. Actions apply
    // synchronously, so pathOf() below re-reads the tree after the previous move.
    int insertAt = ti;
    for( STrack *k : kids ) {
        stimeline::submitActive( new SReparentTrackAction(
            strackpath::pathOf( model_, k ), parentPath, insertAt ) );
        ++insertAt;
    }
    // Delete the now-empty folder (undoable: its restore brings it back, then the
    // child reparents undo back into it).
    const QList<int> fPath = strackpath::pathOf( model_, t );
    if( !fPath.isEmpty() ) {
        stimeline::submitActive( new SRemoveTrackAction( fPath ) );
    }
    if( stack ) stack->endMacro();
}

bool SStdMixerView::tkClickTrackHead( STrack *t, Qt::KeyboardModifiers mods )
{
    if( !t || !model_ ) return false;
    applyTrackSelectionClick( t, mods );
    return true;
}

bool SStdMixerView::tkToggleTrackHead( STrack *t, const QString &which, bool on )
{
    if( !t ) return false;
    for( SSMVMixerControl *mc : *controlArray_ ) {
        if( mc && &mc->getTrack() == t ) return mc->tkClickToggle( which, on );
    }
    return false;      // no head for that track (it is inside a collapsed folder)
}

bool SStdMixerView::tkSendFaderKey( STrack *t, const QString &key )
{
    if( !t ) return false;
    for( SSMVMixerControl *mc : *controlArray_ ) {
        if( mc && &mc->getTrack() == t ) return mc->tkSendFaderKey( key );
    }
    return false;      // no head for that track (it is inside a collapsed folder)
}

bool SStdMixerView::tkDragTrackHead( STrack *t, int targetRow, bool nestOnto )
{
    if( !t ) return false;
    SSMVMixerControl *mc = nullptr;
    for( SSMVMixerControl *c : *controlArray_ ) {
        if( c && &c->getTrack() == t ) { mc = c; break; }
    }
    if( !mc ) return false;
    if( targetRow < 0 || targetRow > rowCount() ) return false;
    if( nestOnto && targetRow >= rowCount() ) return false;

    // What the grip press does before the drag arms: a head outside the
    // selection selects itself first (so it drags alone), one inside it leaves
    // the selection untouched (so the whole block travels).
    if( model_ && !model_->isTrackSelected( t ) )
        applyTrackSelectionClick( t, Qt::NoModifier );

    const int y = nestOnto ? controlYOfRow( targetRow ) + rowHeight( targetRow )/2
                           : controlYOfRow( targetRow );
    beginTrackDrag( mc );
    updateTrackDrag( y );
    // Safe to call straight through here (unlike the mouse handler, which
    // defers it): no widget event is being dispatched, so the rebuild that
    // deletes the heads cannot free one out from under its own handler.
    endTrackDrag( y );
    return true;
}

bool SStdMixerView::groupGesture( STrack *t, bool ungroup )
{
    if( !t || !qContent_ ) return false;
    qContent_->setLastClickTrack( t );
    if( ungroup ) ctUngroupTrack();
    else          ctGroupTrack();
    return true;
}

offset_t SMVActualView::getTimeOf( int x ) const
{
    offset_t totalX = x + getUpperLeftX();
    // Use actual project sample rate, not hardcoded 44100
    int sampleRate = smv_.model_->getProject().getSRate();
    double a = ((double)totalX)/getSecondWidth() * sampleRate;
    return (offset_t)a;
}

// Hit-test the loop-marker grab handles of one clip. Returns the boundary index
// (1 = the first divider) whose handle is under `pos`, or 0 for none. The handle
// geometry comes from scutLoopHandleRect(), the same helper the cut renderer
// draws with, so the visible box and the grabbable box are one and the same.
int SMVActualView::loopMarkerAt( const QPoint &pos, int rowIdx, SLink *clip ) const
{
    if( !clip || !clip->hasStartTime() ) return 0;
    SCut *cut = dynamic_cast<SCut*>( &clip->getSObject() );
    if( !cut || !cut->isLooping() ) return 0;
    length_t seg = cut->getLoopLength().frames();
    if( seg <= 0 ) return 0;
    offset_t start = clip->getStartTime();

    // Handles crowd into each other once a repetition is only a few pixels
    // wide; the renderer stops drawing them there, so stop grabbing them too.
    if( getXPosOfOffset( start + (offset_t) seg ) - getXPosOfOffset( start )
        < 2*SCUT_LOOP_HANDLE_W )
        return 0;

    // Nearest boundary to the pointer, then confirm it is really inside that
    // handle's box. Boundaries at or past the clip end have no divider drawn.
    length_t rel = (length_t) getTimeOf( pos.x() ) - (length_t) start;
    if( rel <= 0 ) return 0;
    int k = (int)( ( rel + seg/2 ) / seg );
    if( k < 1 || (length_t) k * seg >= (length_t) cut->getDuration() ) return 0;

    // The clip's paint rect: the lane inset by one pixel (paintEvent), then by
    // one more by the track renderer that frames each clip.
    QRect clipRect( 0, laneTop( rowIdx )+2, width(), laneHeight( rowIdx )-4 );
    int bx = getXPosOfOffset( start + (offset_t)( (length_t) k * seg ) );
    QRect box = scutLoopHandleRect( clipRect, bx );
    return ( !box.isNull() && box.contains( pos ) ) ? k : 0;
}

// WHICH FADE HANDLE IS UNDER THE POINTER: 1 = fade-in, 2 = fade-out, 0 = none
// (proposal 43 N5 UI). Modelled on loopMarkerAt above, and sharing the SAME
// geometry functions the renderer draws with (`scutFadeHandleX` /
// `scutFadeHandleRect`) -- proposal 41 M7's rule, arrived at after paint and
// hit-test drifted apart for two milestones.
//
// It is tested BEFORE the edge bands, exactly as a loop marker is, but it can
// never steal one: `scutFadeHandleX` parks the handle clear of the band.
int SMVActualView::fadeHandleAt( const QPoint &pos, int rowIdx,
                                 SLink *clip ) const
{
    if( !clip || !clip->hasStartTime() ) return 0;
    SCut *cut = dynamic_cast<SCut*>( &clip->getSObject() );
    if( !cut ) return 0;                       // audio clips only, like the fade
    const length_t dur = cut->getDuration();
    if( dur <= 0 ) return 0;

    const offset_t start = clip->getStartTime();
    const int x0 = getXPosOfOffset( start );
    const int x1 = getXPosOfOffset( start + (offset_t) dur );
    // The clip's paint rect, the same one loopMarkerAt reconstructs.
    QRect clipRect( x0, laneTop( rowIdx )+2, x1-x0, laneHeight( rowIdx )-4 );
    if( clipRect.width() < 2 ) return 0;

    const twClipFade fade = cut->getFade();
    const int inTrue  = getXPosOfOffset( start + (offset_t) fade.inLen );
    const int outTrue = getXPosOfOffset( start + (offset_t)( dur - fade.outLen ) );
    for( int which = 1; which <= 2; ++which ) {
        const int hx = scutFadeHandleX( clipRect, which == 1 ? inTrue : outTrue,
                                        which == 2, SCUT_FADE_EDGE_BAND_PX );
        if( hx < 0 ) return 0;
        QRect box = scutFadeHandleRect( clipRect, hx );
        if( !box.isNull() && box.contains( pos ) ) return which;
    }
    return 0;
}

// proposal 41 D15/M7: THE TAG-FIRST HIT TEST. "Test all tags first, across
// every clip on the lane, then bodies in z-order" (D15) -- so this walks
// EVERY clip on the row, not just whichever one a plain time-based lookup
// would call topmost, and asks STrackRendererInline::tagChipRect() -- the
// SAME geometry draw() paints from -- whether `pos` lands in ITS chip.
//
// A clip's declared tag rect is its full chip width, REGARDLESS of whether a
// later clip's body was painted over part of it afterwards (D11 only
// guarantees the leftmost pixel column stays free of a later clip's body;
// nothing guarantees the rest of a chip that is wide relative to how close
// the next clip starts). Paint order and hit order deliberately differ here
// -- that asymmetry is the whole point of D15: without it, an occluded
// clip's handle would be reachable only through a sliver a few pixels wide,
// which defeats having a generous drag handle at all.
//
// RESOLUTION when more than one clip's tag rect contains `pos` (only
// possible when a later clip starts close enough to reach backward into an
// earlier clip's chip footprint, or at an exact start-time tie):
//
//   - Different start times: the EARLIER clip wins. It is the one that can
//     be occluded (D11 gives a strictly later clip no way to cover the
//     earlier one's own corner), so its whole declared chip stays a valid
//     handle even where a later clip's body/tag was drawn on top of part of
//     it. The later clip is not shortchanged by this: its own body is a much
//     bigger fallback target, reachable via the body pass below wherever its
//     own tag does not also match.
//   - Exact start-time tie (D11's own tiebreak case, unique childIndex): the
//     two rects are PIXEL-IDENTICAL -- there is no "partly free" clip to
//     protect here, so the decision instead follows PAINT order, i.e. the
//     higher childIndex (the one actually drawn on top), so a tied click
//     lands on the clip the user is actually looking at.
//
// Both rules fall out of ONE comparator: ascending by startTime, and within
// an exact tie, descending by childIndex. First match along that order wins.
SLink *SMVActualView::tagHitTestAt( int rowIdx, const QPoint &pos ) const
{
    const STrackRow *row = smv_.rowAt( rowIdx );
    STrack *track = row ? row->track : nullptr;
    if( !track ) return nullptr;
    const int top = laneTop( rowIdx );
    const int lh  = laneHeight( rowIdx );

    SLink *best = nullptr;
    offset_t bestStart = 0;
    int bestIndex = -1;
    int idx = 0;
    for( SLink *lk : track->childLinks() ) {
        const int myIndex = idx++;
        // Nested tracks are their own lanes, never a clip on this one
        // (same skip the paint loop and getTopMostSLinkAt both use).
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) continue;
        if( !lk->hasStartTime() ) continue;
        if( !lk->getSObject().hasDuration() ) continue;   // no extent, no chip

        const offset_t startTime = lk->getStartTime();
        const length_t length = lk->getSObject().getDuration();
        // The SAME startX/endX -> inset-vr mapping strackrndrinline.cpp's
        // draw() computes (there via ctx.getTimeOf()/visibRect, here via
        // getXPosOfOffset() -- the two are the same affine map at
        // visibRect.x()==0, which every lane row paints at).
        const int startX = getXPosOfOffset( startTime );
        const int endX   = getXPosOfOffset( startTime + (offset_t) length );
        QRect vr( startX + 1, top + 1, ( endX - startX ) - 2, lh - 2 );
        if( vr.left() < 0 ) vr.setLeft( 0 );
        if( vr.right() > width() - 1 ) vr.setRight( width() - 1 );
        if( vr.width() < 1 ) continue;

        const QRect chip = STrackRendererInline::tagChipRect( lk->getSObject(), vr );
        if( chip.isEmpty() || !chip.contains( pos ) ) continue;

        const bool better = !best
            || startTime < bestStart
            || ( startTime == bestStart && myIndex > bestIndex );
        if( better ) { best = lk; bestStart = startTime; bestIndex = myIndex; }
    }
    return best;
}

void SMVActualView::updateLastClickVars( const QPoint &pos )
{
    lastClickedStart_ = lastClickedEnd_ = false;
    lastClickedEndUpper_ = false;
    lastClickedStartUpper_ = false;
    lastClickLoopMarker_ = 0;
    lastClickFadeHandle_ = 0;
    lastClickPos_ = pos;
    lastClickTrackIdx_ = rowAtViewY( pos.y() );
    // Y within the clicked lane (its own height, not a global one), used by
    // the upper/lower-half edge gestures below.
    const int laneY = ( lastClickTrackIdx_ >= 0 )
                    ? pos.y() - laneTop( (int) lastClickTrackIdx_ ) : 0;
    const int laneH = ( lastClickTrackIdx_ >= 0 )
                    ? laneHeight( (int) lastClickTrackIdx_ ) : 1;
    lastClickOffset_ = getTimeOf( pos.x() );
    const STrackRow *row = smv_.rowAt( lastClickTrackIdx_ );
    SLink *tlk = row ? row->link : NULL;
    if( tlk ) {
        lastClickTrack_ = row->track;
        // D15/M7 (AC7.1/AC7.2): tags are tested FIRST, across every clip on
        // the lane; only when nothing's tag matches does this fall back to
        // the body test, which is topmost-by-z-order (AC7.3) via
        // STrack::getTopMostSLinkAt.
        lastClickSLink_ = tagHitTestAt( lastClickTrackIdx_, pos );
        if( !lastClickSLink_ )
            lastClickSLink_ = lastClickTrack_->getTopMostSLinkAt( lastClickOffset_ );
        if( lastClickSLink_ ) {
            //qWarning( "Clicked on a %s.\n", lastClickSLink_->getSObject().className() );
            if( lastClickSLink_->hasStartTime() ) {
                offset_t pos = lastClickSLink_->getStartTime();
                int startX = getXPosOfOffset( pos );
                // FIXME: Define the tolerance.
                if( lastClickPos_.x() >= startX
                    && lastClickPos_.x() < (startX+SMV_LEFT_DRAG_PIXEL) ) {
                    lastClickedStart_ = true;
                    // Upper half of the lane → loop backwards; lower half → trim.
                    // Mirrors the right edge.
                    lastClickedStartUpper_ = ( laneY < laneH/2 );
                }
                if( lastClickSLink_->getSObject().hasDuration() ) {
                    length_t len = lastClickSLink_->getSObject().getDuration();
                    lastClickDuration_ = len;
                    int endX = getXPosOfOffset( pos+len );
                    if( lastClickPos_.x() < endX
                        && lastClickPos_.x() >= (endX-SMV_RIGHT_DRAG_PIXEL) ) {
                        lastClickedEnd_ = true;
                        // Upper half of the lane → loop; lower half → extend.
                        lastClickedEndUpper_ = ( laneY < laneH/2 );
                    }
                }
            }
            // A loop-marker handle wins over any edge band it overlaps, so a
            // marker sitting near the clip end still re-tiles rather than
            // extending the clip.
            // A FADE HANDLE outranks every edge band it could overlap, the
            // same way a loop marker does -- though `scutFadeHandleX` parks it
            // so it never actually overlaps one.
            lastClickFadeHandle_ =
                fadeHandleAt( pos, lastClickTrackIdx_, lastClickSLink_ );
            if( lastClickFadeHandle_ > 0 ) {
                lastClickedStart_ = lastClickedEnd_ = false;
                lastClickedEndUpper_ = lastClickedStartUpper_ = false;
            }
            lastClickLoopMarker_ =
                loopMarkerAt( pos, lastClickTrackIdx_, lastClickSLink_ );
            if( lastClickLoopMarker_ > 0 ) {
                lastClickedStart_ = lastClickedEnd_ = lastClickedEndUpper_ = false;
                lastClickedStartUpper_ = false;
            }
        }
    } else {
        lastClickTrack_ = NULL;
        lastClickSLink_ = NULL;
    }    
}

// Move the non-anchor duplicate copies to follow the anchor: each shifts by the
// same time delta (clamped >= 0) and the same lane-row delta (clamped to the
// track list), preserving the group's relative layout. The anchor itself is
// driven by the normal move logic.
void SMVActualView::syncDuplicateGroup()
{
    if( !clipDragIsDuplicate_ || clipDupItems_.size() <= 1 || !lastClickSLink_ )
        return;
    length_t timeDelta =
        (length_t) lastClickSLink_->getStartTime() - (length_t) clipDupAnchorStart_;
    int anchorRow = smv_.rowIndexOfTrack( lastClickTrack_ );
    int rowDelta = ( anchorRow >= 0 && clipDupAnchorRow_ >= 0 )
                   ? anchorRow - clipDupAnchorRow_ : 0;
    int n = smv_.rowCount();
    for( const ClipDupItem &it : clipDupItems_ ) {
        if( !it.copy || it.copy == lastClickSLink_ ) continue;
        length_t ns = (length_t) it.origStart + timeDelta;
        if( ns < 0 ) ns = 0;
        int tr = it.origRow + rowDelta;
        if( tr < 0 ) tr = 0;
        if( tr >= n ) tr = n - 1;
        const STrackRow *row = ( tr >= 0 ) ? smv_.rowAt( tr ) : NULL;
        STrack *dt = row ? row->track : NULL;
        if( dt && dt != dynamic_cast<STrack*>( it.copy->parent() ) )
            it.copy->setParent( dt );
        it.copy->setStartTime( (offset_t) ns );
    }
    update();
}

void SMVActualView::mouseReleaseEvent( QMouseEvent *ev )
{
    // ...and finishes it: revert the live preview, then commit ONE action (P6).
    if( smv_.automationUi().release( *this, ev->pos() ) ) return;

    if( rangeDrag_ != RangeNone ) {
        endRangeDrag( ev->pos().x() );
        return;
    }

    // W2: finalize a warp-marker drag (revert + one undoable action).
    if( markerDragArmed_ ) {
        finishMarkerDrag();
        return;
    }

    // Finalize a clip DUPLICATE (Ctrl-drag): the dragged clips are live copies.
    // Drop the previews and submit an undoable SDuplicateClipAction per copy that
    // re-creates it at its final (snapped) position, followed by ONE selection
    // action naming every copy. The macro is UNCONDITIONAL (AC-a1): a single
    // copy needs it exactly as much as a group does, because the selection
    // change is now itself a second action inside it. Before this the selection
    // was set at ARM time, pointing at the live PREVIEW link (about to be
    // deleted below) — an undo step that outlived the object it named.
    if( clipDragArmed_ && clipDragIsDuplicate_ ) {
        struct Fin { QList<int> src; QList<int> dest; offset_t start; };
        QVector<Fin> fins;
        for( const ClipDupItem &it : clipDupItems_ ) {
            if( !it.copy ) continue;
            STrack *dt = dynamic_cast<STrack*>( it.copy->parent() );
            Fin f;
            f.src   = it.sourcePath;
            f.dest  = dt ? strackpath::pathOf( smv_.getModel(), dt ) : QList<int>();
            f.start = it.copy->getStartTime();   // already snapped by the drag
            fins.append( f );
        }
        for( const ClipDupItem &it : clipDupItems_ )
            if( it.copy ) delete it.copy;         // remove the live previews
        clipDupItems_.clear();
        lastClickSLink_ = NULL;

        QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
        bool macro = !fins.isEmpty() && stack;
        if( macro ) stack->beginMacro( QStringLiteral("Duplicate clips") );
        QList<QList<int>> createdPaths;
        for( const Fin &f : fins ) {
            if( f.src.isEmpty() || f.dest.isEmpty() ) continue;
            QList<int> createdPath;
            SDuplicateClipAction *dup =
                new SDuplicateClipAction( f.src, f.dest, f.start );
            dup->setCreatedPathOut( &createdPath );
            stimeline::submitActive( dup );
            // createdPath is a LOCAL, so it stays safe to read even when
            // apply() rejected and SActionHistory already deleted `dup`.
            if( !createdPath.isEmpty() ) createdPaths.append( createdPath );
        }
        // Select exactly the COPIES — never the originals, never the deleted
        // previews — as part of the SAME macro, so one undo restores both the
        // model and whatever was selected before the drag.
        if( !createdPaths.isEmpty() )
            stimeline::submitActive( new SSetSelectionAction( createdPaths ) );
        if( macro ) stack->endMacro();
        update();
        clipDragArmed_ = false;
        clipDragIsDuplicate_ = false;
        return;
    }

    // FINALIZE A COMP SWIPE (proposal 43 N4). A drag along a take lane comps
    // the region it covered TO THAT TAKE; a plain click (no movement) comps
    // the WHOLE column, which is what a take-lane click has always meant.
    if( compSwipeArmed_ ) {
        compSwipeArmed_ = false;
        const int take = compSwipeTake_;
        compSwipeTake_ = -1;
        STakeStack *stack = takeStackOfLink( lastClickSLink_ );
        if( stack && lastClickTrack_ && lastClickSLink_ && take >= 0 ) {
            QList<int> path =
                strackpath::pathOf( smv_.getModel(), lastClickTrack_ );
            path.append( lastClickTrack_->indexOfChild( lastClickSLink_ ) );
            const QString clipRef = strackpath::pathToString( path );

            const QPoint delta = ev->pos() - lastClickPos_;
            if( qAbs( delta.x() ) <= 4 ) {
                // A CLICK: the whole column, exactly as before. `select-take`
                // clears the map, which is what "this take everywhere" means
                // (proposal 43 N1) -- and its inverse carries the map back.
                stimeline::submitActive( new SSelectTakeAction( path, take ) );
            } else {
                // A SWIPE. Two segments: this take from the swipe's start, and
                // whatever sounded at the END restored just after it, so the
                // comp changes ONLY over the region the pointer covered.
                const offset_t clipStart = lastClickSLink_->getStartTime();
                offset_t a = compSwipeFrom_;
                offset_t b = smv_.alignTime( getTimeOf( ev->pos().x() ) );
                if( b < a ) std::swap( a, b );
                const length_t dur = lastClickSLink_->getSObject().getDuration();
                // Plain conditionals, NOT std::max/std::min: those return a
                // REFERENCE, and with an explicit template argument any
                // operand that needs a conversion binds to a temporary that
                // dies at the end of the full expression. Measured here: `lo`
                // read 96000 on one run and 4181882645476535168 on the next,
                // from the same binary and the same input.
                const offset_t clipEnd = clipStart + (offset_t) dur;
                const offset_t lo = ( a > clipStart ) ? a : clipStart;
                const offset_t hi = ( b < clipEnd )   ? b : clipEnd;
                if( hi > lo ) {
                    // COLUMN-relative: the map's domain is the column's own,
                    // which is the take windows' domain and NOT the timeline's.
                    const int64_t relA = (int64_t) ( lo - clipStart );
                    const int64_t relB = (int64_t) ( hi - clipStart );
                    const int after = stack->takeIndexAt( relB );
                    QUndoStack *ustack =
                        SApplication::app().actionHistory()->undoStack();
                    // ONE undo step for one gesture.
                    if( ustack ) ustack->beginMacro(
                        QStringLiteral( "Comp take" ) );
                    stimeline::submitActive(
                        new SCompMapAction( SCompMapAction::Op::SetSegment,
                                            clipRef, relA, take, 0, 0 ) );
                    if( after >= 0 && after != take
                        && relB < (int64_t) dur )
                        stimeline::submitActive(
                            new SCompMapAction( SCompMapAction::Op::SetSegment,
                                                clipRef, relB, after, 0, 0 ) );
                    if( ustack ) ustack->endMacro();
                }
            }
            update();
        }
        return;
    }

    // Finalize a FADE drag: revert the live shape, then re-apply it as ONE
    // undoable `set-clip-fade` (proposal 43 N5 UI). Same revert-then-act rule
    // every other gesture here follows -- skip the revert and the action finds
    // nothing to change, so its undo step is a no-op and a redo double-applies.
    if( clipDragArmed_ && clipDragIsFade_ && lastClickSLink_ ) {
        const ClipEditTarget t = clipEditTargetOf( lastClickSLink_ );
        if( t.cut && t.cut->getFade() != clipFade0_ ) {
            const twClipFade next = t.cut->getFade();
            t.cut->setFade( clipFade0_ );          // revert
            QList<int> clipPath =
                strackpath::pathOf( smv_.getModel(), lastClickTrack_ );
            clipPath.append( lastClickTrack_->indexOfChild( lastClickSLink_ ) );
            stimeline::submitActive( new SSetClipFadeAction(
                strackpath::pathToString( clipPath ),
                (qint64) next.inLen, (qint64) next.outLen, next.shape, -1 ) );
            update();
        }
        clipDragArmed_ = false;
        clipDragIsFade_ = false;
        clipDragFadeWhich_ = 0;
        return;
    }

    // Finalize a clip EDGE EDIT (resize / slip / stretch / loop) as a single
    // undoable action. The drag mutated the cut live for feedback; revert to the
    // pre-drag window and re-apply the whole window via SResizeClipAction so it
    // is one undo step (and the audio chain rebuilds exactly once, here).
    if( clipDragArmed_ && lastClickSLink_
        && ( lastClickedStart_ || lastClickedEnd_ || clipDragIsSlip_
             || clipDragIsStretch_ || clipDragIsLoop_ || clipDragIsLoopMarker_
             || clipDragIsLoopStart_ ) ) {
        // Take-lane slip: lastClickSLink_ is still the take STACK's own link
        // (path/position identity); the cut actually edited is the take under
        // the pointer. Every other edge gesture is composite-lane only, so
        // clipDragTakeIndex_ is -1 for all of them.
        // The commit is WINDOW-GENERIC (proposal 42 M1): `win` is what the
        // action's values are read from and reverted on, so an EVENT clip's
        // trim lands as one undoable `resize-clip` exactly as an audio clip's
        // does. `cut` stays only for the two things that are audio-specific --
        // draining the queued window-param events, and the take-slip revert's
        // exact-Fraction anchor setter.
        SCut *cut = nullptr;
        SClipWindow *win = nullptr;
        if( clipDragTakeIndex_ >= 0 ) {
            STakeStack *stack = takeStackOfLink( lastClickSLink_ );
            SClipWindow *take = stack ? stack->takeAt( clipDragTakeIndex_ ) : nullptr;
            cut = take ? dynamic_cast<SCut*>( &take->asObject() ) : nullptr;
            win = take;
        } else {
            // The SAME resolve the drag used (proposal 42 M1). The bare
            // `dynamic_cast<SCut*>( &lastClickSLink_->getSObject() )` this
            // replaced was NULL for a direct take column, so a border drag on
            // one committed NOTHING: the live mutation stood with no action
            // and no undo step behind it.
            cut = clipEditTargetOf( lastClickSLink_ ).cut;
            win = clipEditTargetOf( lastClickSLink_ ).win;
        }
        if( cut ) win = SClipWindow::of( &cut->asObject() );
        if( win ) {
            offset_t newStart   = lastClickSLink_->getStartTime();
            Fraction newAnchor  = win->contentAnchorExact();
            length_t newDur     = win->duration();
            length_t newLoop    = win->loopLength();
            Fraction newStretch = clipStretch0_;
            // A TAKE-LANE SLIP COMMITS EXACTLY ONE FACT: the take's source
            // anchor. Everything else in the action is the PLACEMENT's
            // geometry, which the gesture did not touch -- so it must be read
            // from the object it will be WRITTEN to (the link's own window),
            // never from the take.
            //
            // Reading it from the take is what made an Alt-drag on a WRAPPED
            // column corrupt it: the take's duration (the stack's raw length)
            // went into the action as the placement's, and `lastClickDuration_`
            // -- snapshotted from the LINK's object -- is the wrapper's, so
            // `changed` was true even for a zero-pixel drag and the two
            // durations SWAPPED on every drag. Measured on a 2 s wrapper over
            // 4 s takes: one drag made the clip 4 s long and moved its source
            // window 1.4 s. On the DIRECT shape the two numbers are equal by
            // stakestack.h invariant 1, which is why this never showed there.
            if( clipDragTakeIndex_ >= 0 ) {
                newDur     = lastClickDuration_;   // the LINK's own, unchanged
                newLoop    = clipLoopLen0_;
                newStretch = clipStretch0_;
            }
            if( clipDragIsStretch_ ) {
                // Same exact-ratio computation as the live drag (the values
                // must agree). The SOURCE ANCHOR is invariant under a
                // stretch edit (proposal 18 Phase 3) - no offset rescale.
                Fraction s0 = clipStretch0_ > Fraction(0) ? clipStretch0_
                                                          : Fraction(1);
                Fraction srcSpan = Fraction( lastClickDuration_ ) / s0;
                if( srcSpan < Fraction(1) ) srcSpan = Fraction(1);
                newStretch = ( Fraction( newDur ) / srcSpan )
                                 .limitedTo( (uint64_t)1 << 20 );
                newAnchor = clipSrcStart0_;
            }
            bool changed = newStart != clipDragStart0_ || newAnchor != clipSrcStart0_
                        || newDur != lastClickDuration_ || newLoop != clipLoopLen0_
                        || newStretch != clipStretch0_;
            if( changed ) {
                // Apply queued window parameter events first (before reverting)
                // This safely handles any invalidateCapture calls without lock contention
                if( cut ) cut->processWindowParamEvents();

                // Then revert to pre-drag state and re-apply via action
                lastClickSLink_->setStartTime( clipDragStart0_ );
                if( clipDragTakeIndex_ >= 0 ) {
                    // REVERT ONLY WHAT THE LIVE DRAG MUTATED. The move handler
                    // touches the take's slip and nothing else, so the whole-
                    // window revert below would write the PLACEMENT's duration
                    // onto the take -- the second half of the corruption above,
                    // and the reason the take's own length changed on a drag
                    // that only ever asked for a slip. `setSrcStart` takes the
                    // EXACT pre-drag anchor (a Fraction, not a floored warped
                    // offset), so a take under a non-unity stretch reverts to
                    // the value the inverse action is then built from.
                    if( cut ) cut->setSrcStart( clipSrcStart0_ );
                    else win->setWindowExact( clipSrcStart0_, win->duration(),
                                              win->loopLength(),
                                              win->stretchOrRate() );
                } else {
                    win->setWindowExact( clipSrcStart0_, lastClickDuration_,
                                         clipLoopLen0_, clipStretch0_ );
                }
                QList<int> clipPath = strackpath::pathOf( smv_.getModel(), lastClickTrack_ );
                clipPath.append( lastClickTrack_->indexOfChild( lastClickSLink_ ) );
                stimeline::submitActive(
                    new SResizeClipAction( clipPath, newStart, newAnchor, newDur,
                                           newLoop, newStretch, clipDragTakeIndex_ ) );
                update();
            }
        }
        clipDragArmed_ = false;
        clipDragIsSlip_ = clipDragIsStretch_ = clipDragIsLoop_ = false;
        clipDragIsLoopMarker_ = clipDragIsLoopStart_ = false;
        clipDragTakeIndex_ = -1;
        return;
    }

    // Finalize a clip MOVE as a single undoable action. The drag mutated the
    // model live for feedback; here we revert to the pre-drag placement and
    // re-apply it through SMoveClipAction so it lands as one undo step.
    if( clipDragArmed_ && lastClickSLink_ && clipDragTrack0_
        && !lastClickedStart_ && !lastClickedEnd_ ) {
        SLink *link = lastClickSLink_;
        STrack *destTrack = lastClickTrack_;
        offset_t newStart = link->getStartTime();
        if( destTrack && ( destTrack != clipDragTrack0_ || newStart != clipDragStart0_ ) ) {
            // Revert to the snapshot, then redo via the action.
            if( destTrack != clipDragTrack0_ ) link->setParent( clipDragTrack0_ );
            link->setStartTime( clipDragStart0_ );

            QList<int> clipPath = strackpath::pathOf( smv_.getModel(), clipDragTrack0_ );
            clipPath.append( clipDragTrack0_->indexOfChild( link ) );
            QList<int> destTrackPath = strackpath::pathOf( smv_.getModel(), destTrack );
            stimeline::submitActive(
                new SMoveClipAction( clipPath, destTrackPath, newStart ) );
            update();
        }
    }
    clipDragArmed_ = false;

    // Reposition the cursor whenever this click was NOT consumed by a gesture
    // that already returned above (range/take/marker/edge-resize/stretch/
    // loop/duplicate). That leaves two cases here: a click on empty timeline,
    // and a plain click on a clip BODY with no meaningful move (the clip-move
    // branch above already ran and found nothing changed) — both count as a
    // "click", so both reposition the playhead/cursor to where the user
    // clicked.
    if( rangeDrag_ == RangeNone && !clipDragArmed_ ) {
        // Check that the mouse didn't move significantly (within 4 pixels).
        // This distinguishes a click from a small drag (so we don't seek while
        // editing).
        const int CLICK_THRESHOLD = 4;
        QPoint delta = ev->pos() - lastClickPos_;
        if( delta.manhattanLength() <= CLICK_THRESHOLD ) {
            // No range drag and minimal mouse movement = pure click.
            offset_t ofs = smv_.alignTime( getTimeOf( ev->pos().x() ) );
            if( !smv_.rootName().isEmpty() ) {
                // ARRANGEMENT TAB: `ofs` is a frame of THIS arrangement, and the
                // transport's position is a MASTER frame. Writing one into the
                // other is what made clicking in an asset tab jump the master
                // transport to an unrelated bar — the defect proposal 09 §15
                // exists to remove. So the click parks THIS view's own resting
                // cursor and does not touch the transport.
                //
                // Mapping the click BACK to a master frame was considered and is
                // a non-goal for now: it is ambiguous by construction (which of
                // N placements, and which repetition of a looping one) and
                // impossible when nothing places the arrangement at all. §15
                // records it.
                parkedLocalPos_ = ofs;
                lastWalkMasterPos_ = -1;      // drop the memo: the answer moved
                update();
                return;
            }
            // setGlobalLocatorPos repositions the RUNNING engine too when
            // playing (see SApplication) — a plain model_->seekTo would only
            // move the component cursors, not the playback position.
            SApplication::app().setGlobalLocatorPos( ofs );
            // A direct user navigation (item o): Space must resume from HERE
            // next, not from wherever playback or Stop leaves the locator.
            SApplication::app().noteUserNavigatedLocator( ofs );
        }
    }
}

/**
 * Return the rectangle of the given SObject onscreen. It is assumed to 
 * have start and stop time.
 *
 * The rectangle is not meant to be exact, but clipped at the left and the right 
 * border.
 *
 * FIXME: Remove the 44100.
 */
// Take k of every take stack on the row's track, in one lane (phase 3). The
// active take draws bright with a highlight frame; inactive takes are dimmed
// (click-to-comp lives in mousePressEvent). Plain cuts have no takes and
// appear only on the composite lane.
void SMVActualView::drawTakeLane( QPainter &p, const STrackRow &row,
                                  int /*rowIdx*/, const QRect &laneRect )
{
    // Halved with the rest of the field; still darker than a track lane.
    p.fillRect( laneRect, QColor( 13, 19, 25 ) );
    // THE TRACK'S PALETTE ENTRY (app/model/sclipcolors.h), resolved once for
    // the lane exactly as STrackRendererInline::draw does it for the composite
    // lane above -- the two lanes MUST agree about what colour this track is,
    // and they agree by calling one function rather than by both being grey.
    int colorIndex = row.track->colorIndex();
    if( colorIndex < 0 ) {
        SProject *proj = SAppContext::get().getCurrentProject();
        SObject *croot = proj ? proj->getRootComponent() : nullptr;
        colorIndex = croot
            ? sclipcolors::autoIndexForLane( *croot, *row.track ) : 0;
    }
    const bool laneMuted = row.track->isMuted();

    for( SLink *lk : row.track->childLinks() ) {
        STakeStack *stack = takeStackOfLink( lk );
        if( !stack ) continue;
        // Timeline invariant 2: the canvas does not know clip types. A take
        // lane draws whatever window is on it through the polymorphic renderer
        // path, so an event take paints like an audio one (proposal 37 P1) -
        // the SCut cast this replaced silently drew nothing.
        if( !stack->takeObjectAt( row.takeRow ) )
            continue;                               // this stack has fewer takes
        const offset_t start = lk->getStartTime();
        // The LINK's own object's duration, not the inner stack's: on the
        // legacy SCut-wraps-STakeStack shape lk->getSObject() is the
        // WRAPPING cut, whose own window governs the clip's displayed
        // extent (it may be slipped/trimmed independent of the stack's raw
        // material) -- on the modern shape lk->getSObject() IS the stack,
        // so this is identical to stack->getDuration() and nothing changes.
        const length_t dur = lk->getSObject().getDuration();
        int x0 = getXPosOfOffset( start );
        int x1 = getXPosOfOffset( start + (offset_t)dur );
        if( x1 <= laneRect.left() || x0 >= laneRect.right() ) continue;
        if( x0 < laneRect.left() ) x0 = laneRect.left();
        if( x1 > laneRect.right() ) x1 = laneRect.right();
        QRect vr( x0, laneRect.y()+1, x1-x0, laneRect.height()-2 );
        if( vr.width() < 1 ) continue;

        const bool active = ( stack->activeTakeIndex() == row.takeRow );

        // THE CLIP'S OWN RENDERER, asked for ONE take (SObjectRenderer::
        // drawTake / collectTakeEnvelope). It used to reach past the clip to
        // the take OBJECT and draw it against this bare view context, which
        // dropped every window parameter of a WRAPPING cut on the
        // `SLink -> SCut -> STakeStack` shape: slip, stretch and loop tiling
        // never reached the take lane, so the take was laid out 1:1 from the
        // clip's left edge while the audio played it through the wrapper's
        // window. Measured on a real project: a whole bar out. A take lane is
        // the COMPING surface, so a waveform that does not line up with what
        // plays is not a cosmetic defect. Going through the clip's renderer
        // also restores timeline CONTRACT inv. 2 — the canvas does not know
        // clip types; the take index is the only thing it says.
        InlineRenderContext myctx( *this, p );
        myctx.setVisibRect( vr );
        SObjectRenderer *rndr = lk->getSObject().getInlineRenderer();

        // ...and fill the body BY MATERIAL, exactly as the composite lane has
        // since proposal 41 D10 — the same helper, so the same grey means the
        // same thing on both. The take-lane fill shows through a gap.
        // The track's colour, in this clip's state. A take lane is never
        // "selected" in its own right -- the composite lane carries selection --
        // so only the muted variant applies here; the INACTIVE dimming below is
        // a separate, additive black wash and works on any body colour.
        const QColor bodyColor =
            sclipcolors::body( colorIndex, false, laneMuted );
        // ...and hand that pair down to the take's own renderer, so a take
        // waveform is the same colour as the one on the composite lane above.
        {
            SClipColors cc;
            cc.body = bodyColor;
            cc.wave = sclipcolors::wave( colorIndex, false, laneMuted );
            myctx.setClipColors( cc );
        }
        const bool paintedByMaterial = rndr
            && fillBodyByMaterial( p, vr, bodyColor, myctx,
                   [&]( const SEnvelopeWindow &w, preview_t *o ) {
                       return rndr->collectTakeEnvelope( *lk, w, o,
                                                         row.takeRow );
                   } );
        if( !paintedByMaterial )
            p.fillRect( vr, bodyColor );

        if( rndr )
            rndr->drawTake( *lk, myctx, row.takeRow );
        // WHERE THIS TAKE IS COMPED, not just WHETHER it is active (proposal
        // 43 N4). With no map that is the whole clip or none of it, exactly as
        // before -- `takeIndexAt` folds the degenerate case in, so this loop
        // reduces to the old two-branch paint on every project that has no
        // comp. With a map it is per REGION, which is the only honest way to
        // draw a lane whose take sounds in some places and not others.
        const twCompMap &cmap = stack->compMap();
        const offset_t clipEndPos = start + (offset_t) dur;
        offset_t segPos = start;
        while( segPos < clipEndPos ) {
            // The run this take's state holds for: the next boundary, or the
            // clip's end. Positions are COLUMN-relative for the map and
            // TIMELINE for the paint, and `start` is the difference.
            offset_t nextPos = clipEndPos;
            for( const twCompSegment &cs : cmap.segments() ) {
                const offset_t at = start + (offset_t) cs.at;
                if( at > segPos ) { nextPos = std::min( nextPos, at ); break; }
            }
            const bool on =
                ( stack->takeIndexAt( (int64_t) ( segPos - start ) )
                  == row.takeRow );
            int rx0 = getXPosOfOffset( segPos );
            int rx1 = getXPosOfOffset( nextPos );
            if( rx0 < vr.left() )  rx0 = vr.left();
            if( rx1 > vr.right() + 1 ) rx1 = vr.right() + 1;
            if( rx1 > rx0 ) {
                QRect rr( rx0, vr.y(), rx1 - rx0, vr.height() );
                if( on ) {
                    p.setPen( QColor( 240, 220, 80 ) );
                    p.drawRect( rr.adjusted( 0, 0, -1, -1 ) );
                } else {
                    p.fillRect( rr, QColor( 0, 0, 0, 130 ) );   // dim
                }
            }
            if( nextPos <= segPos ) break;
            segPos = nextPos;
        }
    }
}

QRect SMVActualView::getSLinkVisibRect( int trackIdx, const SLink &lk )
{
    // Repaint rects are view space: the lane's own top and height, scroll
    // included (it was missing here, so a scrolled drag repainted the wrong
    // band).
    QRect r( 0, laneTop( trackIdx ), rect().width(), laneHeight( trackIdx ) );
    if( !lk.hasStartTime() ) {
        return r;
    }
    offset_t startTimeOfs = lk.getStartTime();
    int srate = smv_.model_ ? smv_.model_->getProject().getSRate() : 48000;
    double startTime = ((double)startTimeOfs)/srate;
    int startPos = (int)(startTime*secondWidth_);
    startPos -= upperLeftX_;
    if( startPos>=0 && startPos<width() ) r.setLeft( startPos );
    if( lk.getSObject().hasDuration() ) {
        startTimeOfs += lk.getSObject().getDuration();
        startTime = ((double)startTimeOfs)/srate;
        startPos = (int)(startTime*secondWidth_);
        startPos -= upperLeftX_;
        if( startPos>0 && startPos<width() ) r.setRight( startPos );
    }
    return r;
}

// ---------------------------------------------------------------------------
// Time-range selection (shown in the top ruler band)
// ---------------------------------------------------------------------------

void SMVActualView::rangeBounds( offset_t &lo, offset_t &hi ) const
{
    if( rangeStart_ <= rangeEnd_ ) { lo = rangeStart_; hi = rangeEnd_; }
    else                           { lo = rangeEnd_;   hi = rangeStart_; }
}

offset_t SMVActualView::getRangeStart() const
{
    offset_t lo, hi; rangeBounds( lo, hi ); return lo;
}

offset_t SMVActualView::getRangeEnd() const
{
    offset_t lo, hi; rangeBounds( lo, hi ); return hi;
}

void SMVActualView::beginRangeDrag( int x )
{
    if( x < 0 ) x = 0;
    offset_t t = smv_.alignTime( getTimeOf( x ) );

    // If the press lands on an existing end, grab it for moving; on the body
    // (between the ends) grab the whole selection; otherwise start a brand-new
    // range (this press fixes one end). Use normalized bounds for the pixels so
    // edge vs. body is consistent regardless of end ordering.
    if( rangeValid_ ) {
        int xs = getXPosOfOffset( getRangeStart() );
        int xe = getXPosOfOffset( getRangeEnd() );
        if( qAbs( x - xs ) <= SMV_RANGE_GRAB_PIXEL ) { rangeDrag_ = RangeMoveStart; return; }
        if( qAbs( x - xe ) <= SMV_RANGE_GRAB_PIXEL ) { rangeDrag_ = RangeMoveEnd;   return; }
        if( x > xs + SMV_RANGE_GRAB_PIXEL && x < xe - SMV_RANGE_GRAB_PIXEL ) {
            rangeDrag_            = RangeMove;
            rangeMovePressT_      = t;               // aligned press time
            rangeMoveAnchorStart_ = getRangeStart();
            rangeMoveAnchorEnd_   = getRangeEnd();
            return;
        }
    }
    rangeStart_ = rangeEnd_ = t;
    rangeValid_ = true;
    rangeDrag_ = RangeCreate;
    update();
}

void SMVActualView::updateRangeDrag( int x )
{
    if( x < 0 ) x = 0;
    offset_t t = smv_.alignTime( getTimeOf( x ) );
    if( rangeDrag_ == RangeMove ) {
        // Shift both ends by the drag delta, preserving length; clamp at 0.
        // offset_t is unsigned, so compute the delta in signed 64-bit.
        long long delta = (long long) t - (long long) rangeMovePressT_;
        long long ns = (long long) rangeMoveAnchorStart_ + delta;
        long long ne = (long long) rangeMoveAnchorEnd_   + delta;
        if( ns < 0 ) { ne -= ns; ns = 0; }
        rangeStart_ = (offset_t) ns; rangeEnd_ = (offset_t) ne;
        update();
        return;
    }
    if( rangeDrag_ == RangeMoveStart ) rangeStart_ = t;
    else                               rangeEnd_   = t;   // RangeCreate or RangeMoveEnd
    update();
}

void SMVActualView::endRangeDrag( int x )
{
    updateRangeDrag( x );
    if( rangeStart_ > rangeEnd_ ) {                   // normalize
        offset_t tmp = rangeStart_; rangeStart_ = rangeEnd_; rangeEnd_ = tmp;
    }
    // A click with no drag (zero-length create) clears the selection.
    if( rangeDrag_ == RangeCreate && rangeStart_ == rangeEnd_ ) {
        rangeValid_ = false;
    }
    rangeDrag_ = RangeNone;
    saveRangeToProject();
    update();
}

void SMVActualView::saveRangeToProject()
{
    SProject &p = smv_.model_->getProject();
    p.setProp( SProjectProps::RangeValid, rangeValid_ );
    p.setProp( SProjectProps::RangeStart, (qulonglong) rangeStart_ );
    p.setProp( SProjectProps::RangeEnd,   (qulonglong) rangeEnd_ );
}

void SMVActualView::loadRangeFromProject()
{
    SProject &p = smv_.model_->getProject();
    rangeValid_ = p.prop( SProjectProps::RangeValid, false ).toBool();
    rangeStart_ = (offset_t) p.prop( SProjectProps::RangeStart, (qulonglong) 0 ).toULongLong();
    rangeEnd_   = (offset_t) p.prop( SProjectProps::RangeEnd,   (qulonglong) 0 ).toULongLong();

    if (!rangeValid_) {
        STimeGridSpec tgs = smv_.getTimeGridSpec();
        double beatSec = tgs.getTimeGridWidth();
        int bpb = tgs.getEmphasizeGrids(0);
        int srate = smv_.model_->getProject().getSRate();

        offset_t barDurationSamples = (offset_t)(beatSec * bpb * srate);
        rangeStart_ = 4 * barDurationSamples;
        rangeEnd_ = rangeStart_ + 4 * barDurationSamples;
        rangeValid_ = true;
    }
}

// --- zoom / horizontal pan persistence (fix/track-list-polish m) ----------
// Same shape as saveRangeToProject()/loadRangeFromProject() just above: a
// generic project property (SProjectProps), read once at construction
// (the view is rebuilt per project) and written on every change so a save
// always sees the latest value.

void SMVActualView::saveViewStateToProject()
{
    if( !smv_.model_ ) return;
    SProject &p = smv_.model_->getProject();
    p.setProp( SProjectProps::TimelineZoomSecondWidth, secondWidth_ );
    p.setProp( SProjectProps::TimelineScrollX, (qulonglong) upperLeftOffset_ );
}

void SMVActualView::loadViewStateFromProject()
{
    SProject &p = smv_.model_->getProject();
    secondWidth_ = p.prop( SProjectProps::TimelineZoomSecondWidth, 30.0 ).toDouble();
    if( secondWidth_ < 0.000001 ) secondWidth_ = 0.000001;   // setSecondWidth's own clamp
    upperLeftOffset_ = (offset_t) p.prop(
        SProjectProps::TimelineScrollX, (qulonglong) 0 ).toULongLong();
    // Keep upperLeftX_ (pixel space) consistent with upperLeftOffset_ (frame
    // space) exactly the way setSecondWidth() does — the same formula, so a
    // restored zoom+pan pair maps to the same pixel origin a fresh
    // setSecondWidth(secondWidth_) would have produced.
    int srate = p.getSRate();
    upperLeftX_ = (int) ( ( (double) upperLeftOffset_ ) / srate * secondWidth_ );
}

void SMVActualView::drawRange( QPainter &p, const QRect &myRect )
{
    if( !rangeValid_ ) return;
    offset_t lo, hi; rangeBounds( lo, hi );
    int xlo = getXPosOfOffset( lo );
    int xhi = getXPosOfOffset( hi );

    // Grey band in the upper half of the ruler.
    if( xhi > xlo ) {
        int rulerMid = SMV_TIME_RULER_HEIGHT / 2;
        p.fillRect( QRect( xlo, 0, xhi - xlo, rulerMid ),
                    QColor( 150, 150, 150 ) );
    }
    // Edges as vertical lines over all tracks.
    p.setPen( QColor( 80, 80, 80 ) );
    if( xlo >= 0 && xlo < myRect.width() ) p.drawLine( xlo, 0, xlo, myRect.height()-1 );
    if( xhi >= 0 && xhi < myRect.width() ) p.drawLine( xhi, 0, xhi, myRect.height()-1 );
}

void SMVActualView::drawRulerTicks( QPainter &p, const QRect &myRect )
{
    // Render time markers (beats, bars, or minutes/hours) in the ruler band,
    // choosing granularity to avoid overlaps based on current zoom.
    STimeGridSpec tgs = smv_.getTimeGridSpec();
    double beatSec  = tgs.getTimeGridWidth();      // seconds per beat
    int    bpb      = tgs.getEmphasizeGrids(0);    // beats per bar (e.g. 4)
    bool   barsMode = (smv_.getModel() &&
        smv_.getModel()->getProject().prop(SProjectProps::RulerMode, "bars").toString() == "bars");

    // --- 1. Use a small font so it fits in 16 px ---
    QFont f = font();
    f.setPointSize(7);
    p.setFont(f);
    QFontMetrics fm(f);

    // --- 2. Estimate worst-case label width ---
    int labelW = fm.horizontalAdvance(barsMode ? "9999.4.479" : "99:59:999") + 3;

    // --- 3. Determine which granularity to render ---
    // pixelsPerBeat / pixelsPerBar / pixelsPerMinute / pixelsPerHour
    double beatPx   = beatSec   * secondWidth_;
    double barPx    = beatPx    * bpb;
    double minutePx = 60.0      * secondWidth_;
    double hourPx   = 3600.0    * secondWidth_;

    enum Level { Beat=0, Bar=1, Minute=2, Hour=3 };
    Level level = Hour;
    if      (beatPx   >= labelW) level = Beat;
    else if (barPx    >= labelW) level = Bar;
    else if (minutePx >= labelW) level = Minute;
    // else Hour

    // --- 4. Choose step size in seconds ---
    double stepSec;
    switch (level) {
        case Beat:   stepSec = beatSec;        break;
        case Bar:    stepSec = beatSec * bpb;  break;
        case Minute: stepSec = 60.0;           break;
        case Hour:   stepSec = 3600.0;         break;
    }

    // --- 5. Iterate using integer step count to avoid float drift ---
    //   leftSec = time in seconds of the left pixel edge
    double leftSec = (double)upperLeftX_ / secondWidth_;
    long   firstStep = (long)floor(leftSec / stepSec);  // may be 0 or negative

    // Draw tick colour
    p.setPen(QColor(80, 80, 80));

    for (long n = firstStep; ; ++n) {
        double t    = n * stepSec;               // time in seconds
        int    x    = (int)(t * secondWidth_) - upperLeftX_;
        if (x > myRect.width()) break;
        if (x < -labelW)       continue;

        // --- Format label ---
        QString label;
        if (barsMode) {
            // beat index from project start (0-based)
            double beatIdxExact = t / beatSec;
            long beatIdx  = (long)floor(beatIdxExact);
            long bar      = beatIdx / bpb + 1;     // 1-based bar
            int  beat     = (int)(beatIdx % bpb) + 1; // 1-based beat within bar
            if (level == Beat) {
                double fracBeat = beatIdxExact - beatIdx;  // [0, 1) within the beat
                int tick = (int)round(fracBeat * 480.0);
                if (tick > 0)
                    label = QString("%1.%2.%3").arg(bar).arg(beat).arg(tick, 3, 10, QChar('0'));
                else
                    label = QString("%1.%2").arg(bar).arg(beat);
            } else {
                label = QString::number(bar);
            }
        } else {
            // time mode
            long totalMs = (long)round(t * 1000.0);
            long ms      = totalMs % 1000;  totalMs /= 1000;
            long secs    = totalMs % 60;    totalMs /= 60;
            long mins    = totalMs % 60;    totalMs /= 60;
            long hrs     = totalMs;
            if (hrs > 0)
                label = QString("%1:%2:%3").arg(hrs)
                            .arg(mins, 2, 10, QChar('0'))
                            .arg(secs, 2, 10, QChar('0'));
            else
                label = QString("%1:%2:%3").arg(mins)
                            .arg(secs, 2, 10, QChar('0'))
                            .arg(ms,   3, 10, QChar('0'));
        }

        // --- Tick mark height: coarser boundary = taller ---
        int tickH = 3;
        if (level <= Bar  && (long)round(t / (beatSec * bpb)) * (beatSec * bpb) == t) tickH = 6;
        if (minutePx > 20 && fmod(t, 60.0)   < stepSec * 0.01) tickH = 9;
        if (hourPx   > 20 && fmod(t, 3600.0) < stepSec * 0.01) tickH = 12;

        p.drawLine(x, 0, x, tickH);
        if (x + 1 + labelW <= myRect.width())
            p.drawText(x + 2, SMV_TIME_RULER_HEIGHT - 2, label);
    }
}

void SMVActualView::ctRangeSetBPM()
{
    bool ok = false;
    STimeGridSpec tgs( smv_.getTimeGridSpec() );
    double oldTempo = tgs.getBPM();
    double newTempo = QInputDialog::getDouble(
        &smv_, "Smaragd request", tr( "Please enter new BPM" ),
        oldTempo, 10., 4000., 1, &ok );
    if( ok && newTempo != oldTempo ) {
        // Through the verb, never the project: set-tempo is the ONLY tempo
        // write (proposal 37 D2). It also re-derives every beats-timebase
        // link, which a bare project write would silently skip, and it is
        // what puts a tempo change on the undo stack at all.
        stimeline::submitActive( new SSetTempoAction( newTempo ) );
    }
}

void SMVActualView::ctRangeClear()
{
    rangeValid_ = false;
    rangeDrag_ = RangeNone;
    saveRangeToProject();
    update();
}

void SMVActualView::ctCreateAssetFromTrack()
{
    // CREATING AN ASSET ALWAYS MAKES AN ARRANGEMENT (requester, 2026-08-20).
    // The old behaviour windowed the clicked track IN PLACE and left it in the
    // master, so the same audio was then heard twice -- once where it lived and
    // once at the placement -- and the asset had no home of its own to edit in.
    // Extraction moves the selected tracks OUT into an arrangement of their own
    // and puts one clip where they were, so the material is heard exactly once
    // and double-clicking that clip drills into it.
    //
    // It acts on the SELECTION, not on lastClickTrack_ alone. That was the one
    // track operation in this file that did not: every other one uses
    // pruneNestedTargets( selectionTargets( ... ) ), and "make an asset from
    // these four lanes" is the normal case rather than the exotic one.
    if( !rangeValid_ || !lastClickTrack_ || !smv_.model_ ) return;
    offset_t t0 = getRangeStart();
    offset_t t1 = getRangeEnd();
    if( t1 <= t0 ) return;

    const QList<STrack *> targets =
        SStdMixerView::pruneNestedTargets( smv_.selectionTargets( lastClickTrack_ ) );
    if( targets.isEmpty() ) return;

    QStringList paths;
    for( STrack *t : targets )
        paths << strackpath::pathToString( strackpath::pathOf( smv_.model_, t ) );

    // The name is ASKED FOR, prefilled with the one the action would generate,
    // because it becomes the TAB LABEL and a tab called "Arrangement 3" is not
    // worth having. Same dialog shape as ctRangeSetBPM. The prompt names the
    // destructive half explicitly -- the tracks MOVE -- because that is the
    // part a user cannot undo by looking at the result.
    SProject &proj = smv_.model_->getProject();
    QString suggestion;
    for( int n = 1; ; ++n ) {
        suggestion = QString( "Arrangement %1" ).arg( n );
        if( !proj.hasArrangement( suggestion ) && !proj.hasAsset( suggestion ) )
            break;
    }
    bool ok = false;
    const QString name = QInputDialog::getText(
        &smv_, tr( "Create asset from range" ),
        tr( "%1 track(s) will MOVE into a new arrangement of their own, and one "
            "clip windowing the selected range takes their place.\n\nName:" )
            .arg( targets.size() ),
        QLineEdit::Normal, suggestion, &ok );
    if( !ok || name.trimmed().isEmpty() ) return;

    stimeline::submitActive( new SExtractArrangementAction(
        paths.join( ';' ), t0, t1, name.trimmed(),
        QStringLiteral( "range" ), t0 ) );
}

// Set the mouse cursor to telegraph the clip-edit gesture under the pointer,
// given the current keyboard modifiers: resize/extend (SizeHor), time-stretch
// (SplitH, Ctrl+border), loop (custom ↻, right-edge upper half), slip (SizeAll,
// Alt+body), duplicate (DragCopy, Ctrl+body), move (OpenHand). Off any clip it
// is the plain arrow. Read-only — does not touch the lastClick* drag state.
void SMVActualView::updateHoverCursor( const QPoint &pos, Qt::KeyboardModifiers mods )
{
    static QCursor *s_loopCursor = NULL;
    if( !s_loopCursor ) {
        QPixmap pm( 24, 24 );
        pm.fill( Qt::transparent );
        QPainter pp( &pm );
        pp.setRenderHint( QPainter::Antialiasing, true );
        pp.setPen( QPen( QColor( 0, 0, 0 ), 2 ) );
        pp.drawArc( 5, 5, 14, 14, 50*16, 280*16 );   // open circle
        pp.setBrush( QColor( 0, 0, 0 ) );
        QPolygon tri;
        tri << QPoint( 18, 3 ) << QPoint( 23, 9 ) << QPoint( 14, 8 );
        pp.drawPolygon( tri );                        // arrow head
        pp.end();
        s_loopCursor = new QCursor( pm, 12, 12 );
    }

    Qt::CursorShape shape = Qt::ArrowCursor;
    const QCursor *custom = NULL;
    QString mode;   // status-bar label; empty = idle

    if( pos.y() >= SMV_TIME_RULER_HEIGHT ) {
        int rowIdx = rowAtViewY( pos.y() );
        const STrackRow *row = smv_.rowAt( rowIdx );
        STrack *track = row ? row->track : NULL;
        SLink *clip = track ? track->getTopMostSLinkAt( getTimeOf( pos.x() ) ) : NULL;
        if( clip && clip->hasStartTime()
            && !dynamic_cast<STrack*>( &clip->getSObject() ) ) {
            offset_t st = clip->getStartTime();
            int startX = getXPosOfOffset( st );
            bool onLeft = ( pos.x() >= startX && pos.x() < startX + SMV_LEFT_DRAG_PIXEL );
            bool onRight = false, upper = false;
            if( clip->getSObject().hasDuration() ) {
                length_t len = clip->getSObject().getDuration();
                int endX = getXPosOfOffset( st + (offset_t) len );
                onRight = ( pos.x() < endX && pos.x() >= endX - SMV_RIGHT_DRAG_PIXEL );
                upper = ( pos.y() - laneTop( rowIdx ) < laneHeight( rowIdx )/2 );
            }
            bool onBorder = onLeft || onRight;
            bool ctrl = hasPrimaryMod( mods );   // ⌘ on macOS, Ctrl elsewhere
            bool alt  = mods & Qt::AltModifier;
            // A FADE HANDLE is tested FIRST, matching the press: proposal 43
            // N5's UI made it outrank every edge band AND the loop marker
            // (`clipDragIsLoopMarker_` is gated on `!clipDragIsFade_`), so a
            // cursor that reported anything else here would be promising a
            // gesture the press will not perform.
            if( int fh = fadeHandleAt( pos, rowIdx, clip ) )
                                       { shape = Qt::SizeHorCursor;
                                         mode = ( fh == 1 ) ? "Fade in"
                                                            : "Fade out"; }
            else if( loopMarkerAt( pos, rowIdx, clip ) > 0 )
                                       { shape = Qt::SizeHorCursor;    mode = "Loop length"; }
            else if( onBorder && ctrl ){ shape = Qt::SplitHCursor;     mode = "Time-stretch"; }
            else if( onRight && upper ){ custom = s_loopCursor;        mode = "Loop"; }
            else if( onLeft && upper ) { custom = s_loopCursor;        mode = "Loop back (whole cycles)"; }
            else if( onLeft )          { shape = Qt::SizeHorCursor;    mode = "Trim start"; }
            else if( onRight )         { shape = Qt::SizeHorCursor;    mode = "Extend"; }
            else if( alt )             { shape = Qt::SizeAllCursor;    mode = "Slip"; }
            else if( ctrl )            { shape = Qt::DragCopyCursor;   mode = "Duplicate"; }
            else                       { shape = Qt::OpenHandCursor;   mode = "Move"; }
        }
    }
    if( custom ) setCursor( *custom );
    else setCursor( QCursor( shape ) );

    SApplication::app().setStatusMode( mode );
}

// Proposal 41 D14 (M6): the tag chip's density ladder ANNOUNCES its own cap
// (the level meter's describe()/tooltip precedent) -- whenever a clip draws
// less than its true full name, hovering it must reveal that name. The
// canvas paints clips as rects into ONE QPainter, not as per-clip QWidgets,
// so there is no setToolTip() to hang this off; Qt's own answer for a
// position-dependent tooltip on a plain widget is to override event() for
// QEvent::ToolTip (QWidget::event's docs). The clip lookup reuses the SAME
// hit test the cursor/status-bar code above already uses
// (rowAtViewY + STrack::getTopMostSLinkAt) -- a read-only lookup for a
// tooltip, not the tag-as-drag-handle hit-test order proposal 41 D15/M7
// specifies (that governs which clip a PRESS lands on; this only decides
// what text to show under the pointer). The width/text decision recomputes
// STrackRendererInline::tagFullText()/tagDensityText() -- the SAME shared
// functions draw() calls -- against the clip's own painted rect
// (getSLinkVisibRect, the same rect repaint invalidation already uses), so
// this can never disagree with what is actually on screen without caching
// anything.
bool SMVActualView::event( QEvent *ev )
{
    if( ev->type() != QEvent::ToolTip ) return QWidget::event( ev );

    QHelpEvent *he = static_cast<QHelpEvent *>( ev );
    const QPoint pos = he->pos();
    if( pos.y() >= SMV_TIME_RULER_HEIGHT ) {
        const int rowIdx = rowAtViewY( pos.y() );
        const STrackRow *row = smv_.rowAt( rowIdx );
        STrack *track = row ? row->track : nullptr;
        SLink *clip = track ? track->getTopMostSLinkAt( getTimeOf( pos.x() ) )
                             : nullptr;
        if( clip && clip->hasStartTime()
            && !dynamic_cast<STrack *>( &clip->getSObject() ) ) {
            const QString fullName =
                STrackRendererInline::tagFullText( clip->getSObject() );
            if( !fullName.isEmpty() ) {
                const QRect vr = getSLinkVisibRect( rowIdx, *clip );
                QFont f = QGuiApplication::font();   // matches the tag's
                f.setPointSize( 7 );                 // own font exactly
                const QFontMetrics fm( f );
                bool cut = false;
                STrackRendererInline::tagDensityText(
                    fullName, fm, vr.width() - 2 * 3, &cut );
                if( cut ) {
                    QToolTip::showText( he->globalPos(), fullName, this );
                    return true;
                }
            }
        }
    }
    QToolTip::hideText();
    ev->ignore();
    return true;
}

/**
 * Mouse was moved. Look, if the left button currently is pressed, and if
 * an object was selected initially. If it was, move it.
 */
void SMVActualView::mouseMoveEvent( QMouseEvent *ev )
{
    // An armed automation gesture (P6) consumes the move.
    if( smv_.automationUi().move( *this, ev->pos() ) ) return;

    // Range selection drag takes precedence over clip editing.
    if( rangeDrag_ != RangeNone ) {
        updateRangeDrag( ev->pos().x() );
        return;
    }

    // W2: an armed warp-marker drag consumes the move.
    if( markerDragArmed_ && ( ev->buttons() & Qt::LeftButton ) ) {
        updateMarkerDrag( ev );
        return;
    }

    // No button held: this is a hover — just update the gesture cursor.
    if( !( ev->buttons() & Qt::LeftButton ) ) {
        updateHoverCursor( ev->pos(), ev->modifiers() );
        return;
    }

    // Check scrolling, if the event position is invisible. AC-g1: dragging
    // past either edge is a hand-driven pan exactly as the wheel and the
    // scrollbar are, so it arms the same hold.
    QRect myRect = rect();
    int srate = smv_.model_ ? smv_.model_->getProject().getSRate() : 48000;
    if( ev->pos().x()<0 ) {
        int currentOffset = upperLeftX_;
        int d = -ev->pos().x();
        currentOffset -= d;
        if( currentOffset<0 ) currentOffset = 0;
        if( currentOffset != upperLeftX_ ) {
            armFollowHold();
            setLeftOffset( (offset_t)( ((double)currentOffset)/secondWidth_*srate) );
        }
    } else if( ev->pos().x()>=myRect.width() ) {
        int currentOffset = upperLeftX_;
        int d = ev->pos().x()-myRect.width();
        currentOffset += d;
        if( currentOffset != upperLeftX_ ) {
            armFollowHold();
            setLeftOffset( (offset_t)( ((double)currentOffset)/secondWidth_*srate) );
        }
    }

    // Ge the current track.
    int newTrackIdx = rowAtViewY( ev->pos().y() );
    const STrackRow *newRow = smv_.rowAt( newTrackIdx );
    STrack *newTrack = newRow ? newRow->track : NULL;
    if( newTrack && newTrack == lastClickTrack_ ) newTrack = NULL;

    // Determine which action to take.
    if( ev->buttons() & Qt::LeftButton ) {        
        if( lastClickSLink_ ) {
            // A COMP SWIPE OWNS THE GESTURE and mutates nothing live
            // (proposal 43 N4). Without this the drag falls through to the
            // final `else if( delta != 0 )` branch below, which MOVES THE
            // CLIP -- and does it from `getLastClickStartOffset()`, which a
            // take-lane press never set. Measured: the column's `startTime_`
            // went from 0 to 4467570023102409344 between press and release,
            // which is what a swipe would have done to a user's arrangement.
            if( compSwipeArmed_ ) return;

            // ONE resolve for every gesture below (proposal 42 M1). `cut` is
            // null only for an EVENT window, which the branches guard on; it is
            // the placement's own `SCut` for an ordinary clip and for a wrapped
            // column, and the ACTIVE TAKE's for a direct column — so every
            // branch's existing fast path is reached unchanged in all three.
            const ClipEditTarget tgt = clipEditTargetOf( lastClickSLink_ );
            offset_t downTime = getLastClickOffset();
            offset_t nowTime = getTimeOf( ev->pos().x() );
	    length_t delta = (length_t)nowTime-(length_t)downTime;
            length_t newStart = getLastClickStartOffset() + delta;
            if( newStart<0 ) newStart = 0;

            // Live drags below mutate only the fields needed for visual feedback
            // (cheap, no audio rebuild); the release reverts to the snapshot and
            // re-applies the whole window through SResizeClipAction.
            if( clipDragIsFade_ ) {
                // DRAG A FADE HANDLE: the handle marks the fade's END, so its
                // length is the pointer's distance from the clip's own edge.
                // The clip's position and length are untouched -- a fade is a
                // gain shape, not a window edit.
                SCut *cut = tgt.cut;
                if( !cut ) return;              // audio clips only
                const length_t dur = (length_t) cut->getDuration();
                length_t want = ( clipDragFadeWhich_ == 1 )
                    ? (length_t) smv_.alignTime( getTimeOf( ev->pos().x() ) )
                          - (length_t) clipDragStart0_
                    : (length_t) clipDragStart0_ + dur
                          - (length_t) smv_.alignTime( getTimeOf( ev->pos().x() ) );
                if( want < 0 ) want = 0;
                if( want > dur ) want = dur;
                twClipFade next = clipFade0_;
                if( clipDragFadeWhich_ == 1 ) next.inLen = want;
                else                          next.outLen = want;
                if( next != cut->getFade() ) {
                    QRect r = getSLinkVisibRect( lastClickTrackIdx_,
                                                 *lastClickSLink_ );
                    cut->setFade( next );
                    update( r );
                }
            } else if( clipDragIsLoopMarker_ ) {
                // Drag a LOOP MARKER: re-tile the clip so the grabbed boundary
                // k lands under the pointer — segment = (t - clipStart)/k. The
                // clip's duration is untouched, so shortening the segment fits
                // more repetitions into the same clip. Kept strictly below the
                // duration so the clip stays looping (SCut::isLooping) and at
                // least one marker remains to grab.
                SCut *cut = tgt.cut;
                if( !cut ) return;   // an EVENT window: not this gesture's domain
                length_t span = (length_t) smv_.alignTime( getTimeOf( ev->pos().x() ) )
                              - (length_t) clipDragStart0_;
                length_t newSeg = span / lastClickLoopMarker_;
                length_t maxSeg = (length_t) cut->getDuration() - SMV_CUT_MIN_TIME;
                if( newSeg < SMV_CUT_MIN_TIME ) newSeg = SMV_CUT_MIN_TIME;
                if( newSeg > maxSeg ) newSeg = maxSeg;
                if( newSeg >= SMV_CUT_MIN_TIME
                    && WarpedLen( newSeg ) != cut->getLoopLength() ) {
                    QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                    cut->setLoopLengthRaw( WarpedLen( newSeg ) );
                    cut->queueWindowParamEvent( LOOP_LENGTH_CHANGE, (double) newSeg );
                    cut->getPreviewCapture();  // Non-blocking: schedule async revalidation if needed
                    smv_.getModel()->getProject().notifyArrangementChanged();  // Cascade to live assets
                    update( oldRect );
                }
                update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
            } else if( clipDragIsSlip_ ) {
                // Alt-drag the BODY: slide the content under the clip (change the
                // cut's start offset). Position and length stay put; dragging
                // right reveals earlier content.
                SCut *cut = nullptr;
                if( clipDragTakeIndex_ >= 0 ) {
                    // Take-lane slip: lastClickSLink_ is the take STACK's outer
                    // link (kept for position/geometry only — never fed to
                    // ensureSCut, which would misread the stack as "not an SCut
                    // yet" and replace the stack's own link with a bogus one).
                    // The object actually being slipped is the take under the
                    // pointer, resolved fresh every move in case a concurrent
                    // edit removed it.
                    STakeStack *stack = takeStackOfLink( lastClickSLink_ );
                    SClipWindow *take =
                        stack ? stack->takeAt( clipDragTakeIndex_ ) : nullptr;
                    cut = take ? dynamic_cast<SCut*>( &take->asObject() ) : nullptr;
                    if( !cut ) return;
                } else {
                    cut = tgt.cut;
                    if( !cut ) return;   // an EVENT window has no grain slip
                }
                length_t contentLen = cut->getContent().hasDuration()
                                      ? (length_t) cut->getContent().getDuration() : -1;
                length_t d = (length_t) smv_.alignTime( getTimeOf( ev->pos().x() ) )
                           - (length_t) smv_.alignTime( (offset_t) getLastClickOffset() );
                length_t newOff = (length_t) clipResizeOffset0_ - d;
                // The slip MAY go negative (proposal 23): the clip then opens
                // with silence and its data starts later. Bounded so at least
                // SMV_CUT_MIN_TIME of content stays inside the clip — the mirror
                // of the maxOff rule below, which lets the tail run into silence.
                // The take path bounds against the TAKE's own length: on the
                // wrapped shape `lastClickDuration_` is the WRAPPER's window
                // and using it here engages the negative-slip clamp early (by
                // the difference between the two, 96000 frames on the test
                // fixture). `maxOff` below was already in the take's domain.
                const length_t slipSpan =
                    ( clipDragTakeIndex_ >= 0 )
                        ? (length_t) cut->getDuration()
                        : (length_t) lastClickDuration_;
                length_t minOff = SMV_CUT_MIN_TIME - slipSpan;
                if( minOff > 0 ) minOff = 0;
                if( newOff < minOff ) newOff = minOff;
                // Bound the window START (output domain) to near the content end,
                // not content_end - window_len: a full-length clip (window ==
                // content) would otherwise have zero slip room. Sliding further
                // simply lets the tail run into silence, which is a valid slip.
                if( contentLen >= 0 ) {
                    // W1: content end mapped through the warp map (equals
                    // contentLen*stretch when no anchors exist).
                    length_t maxOff = (length_t) cut->sourceToWarpedExact(
                                          Fraction( (int64_t) contentLen ) )
                                          .floorToInt()
                                    - (length_t) SMV_CUT_MIN_TIME;
                    if( maxOff < 0 ) maxOff = 0;
                    if( newOff > maxOff ) newOff = maxOff;
                }
                QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                // Only rebuild if offset actually changed
                if( WarpedPos( (int64_t)newOff ) != cut->getStartOffset() ) {
                    cut->setStartOffset( (offset_t) newOff );  // Visual feedback + queues event via invalidateCapture
                    cut->getPreviewCapture();  // Non-blocking: schedule async revalidation if needed
                    smv_.getModel()->getProject().notifyArrangementChanged();  // Cascade to live assets
                }
                update( oldRect );
                update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
            } else if( clipDragIsStretch_ ) {
                // Ctrl-drag a BORDER: change the timeline length, keeping the same
                // SOURCE window (grain-stretched, pitch preserved). The sample-backed
                // preview maps each pixel to a source sample via (rel + startOffset)/
                // stretch, so all THREE of stretch, startOffset and duration must move
                // together live or the visible/audible source window drifts. We apply
                // them with the raw setters (no invalidateCapture, so no lock
                // contention); the audio chain rebuilds once on release via the
                // SResizeClipAction. Not clamped to content (it stretches).
                SCut *cut = tgt.cut;
                if( !cut ) return;   // an EVENT window: not this gesture's domain
                offset_t m = smv_.alignTime( getTimeOf( ev->pos().x() ) );
                Fraction s0 = clipStretch0_ > Fraction(0) ? clipStretch0_
                                                          : Fraction(1);
                QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                length_t newDur;
                if( lastClickedEnd_ ) {
                    newDur = (length_t) m - (length_t) clipDragStart0_;
                    if( newDur < SMV_CUT_MIN_TIME ) newDur = SMV_CUT_MIN_TIME;
                } else {
                    offset_t end0 = clipDragStart0_ + (offset_t) lastClickDuration_;
                    offset_t rStart = m;
                    if( (length_t) end0 - (length_t) rStart < SMV_CUT_MIN_TIME )
                        rStart = end0 - (offset_t) SMV_CUT_MIN_TIME;
                    newDur = (length_t) end0 - (length_t) rStart;
                    lastClickSLink_->setStartTime( rStart );
                }
                // The stretch is BORN here as a ratio of two integer frame
                // counts: newDur / srcSpan where srcSpan = duration0 / s0.
                // Computed exactly and denominator-capped ONCE at creation
                // (proposal 18 Phase 2); repeated re-stretching composes
                // rationally instead of drifting through doubles. The SOURCE
                // ANCHOR is authoritative and invariant under a stretch edit
                // (Phase 3), so no offset rescale happens here at all - the
                // derived warped offset follows the stretch automatically.
                Fraction srcSpan = Fraction( lastClickDuration_ ) / s0;
                if( srcSpan < Fraction(1) ) srcSpan = Fraction(1);
                Fraction newStretch = ( Fraction( newDur ) / srcSpan )
                                          .limitedTo( (uint64_t)1 << 20 );
                cut->setStretchRaw( newStretch );
                cut->setDurationRaw( ClipLen( newDur ) );
                cut->getPreviewCapture();  // Non-blocking: schedule async revalidation if needed
                smv_.getModel()->getProject().notifyArrangementChanged();  // Cascade to live assets
                update( oldRect );
                repaint( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
            } else if( clipDragIsLoop_ ) {
                // Drag the RIGHT edge's UPPER half: extend the clip past its
                // content by repeating the previously visible cut. Capture the
                // loop segment once, then grow the total duration.
                SCut *cut = tgt.cut;
                if( !cut ) return;   // an EVENT window: not this gesture's domain
                if( clipLoopSeg_ <= 0 ) {
                    // Capture the segment to repeat once: the previously visible
                    // cut (original loop length if already looping, else the
                    // content the clip showed, capped at the content end).
                    length_t seg = clipLoopLen0_;
                    if( seg <= 0 ) {
                        length_t contentLen = cut->getContent().hasDuration()
                                              ? (length_t) cut->getContent().getDuration() : -1;
                        seg = lastClickDuration_;
                        if( contentLen >= 0
                            && seg > contentLen - (length_t) clipResizeOffset0_ )
                            seg = contentLen - (length_t) clipResizeOffset0_;
                    }
                    if( seg < SMV_CUT_MIN_TIME ) seg = SMV_CUT_MIN_TIME;
                    clipLoopSeg_ = seg;
                }
                offset_t rEnd = smv_.alignTime( getTimeOf( ev->pos().x() ) );
                length_t newDur = (length_t) rEnd - (length_t) clipDragStart0_;
                if( newDur < SMV_CUT_MIN_TIME ) newDur = SMV_CUT_MIN_TIME;
                QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                // Only rebuild if duration actually changed
                if( newDur != cut->getDuration()
                    || WarpedLen( clipLoopSeg_ ) != cut->getLoopLength() ) {
                    cut->setLoopLengthRaw( WarpedLen( clipLoopSeg_ ) );
                    cut->setDuration( newDur );
                    cut->queueWindowParamEvent( LOOP_LENGTH_CHANGE, (double) clipLoopSeg_ );
                    cut->queueWindowParamEvent( DURATION_CHANGE, (double) newDur );
                    cut->getPreviewCapture();  // Non-blocking: schedule async revalidation if needed
                    smv_.getModel()->getProject().notifyArrangementChanged();  // Cascade to live assets
                }
                update( oldRect );
                update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
            } else if( clipDragIsLoopStart_ ) {
                // Drag the LEFT edge's UPPER half: grow (or shrink) the clip
                // backwards in WHOLE loop cycles, keeping every existing
                // repetition on the frame it already plays on. Whole cycles is
                // forced by the model, not a preference: twLoopMap maps
                // clip-relative p to base + (p mod len), so a shift of k*len is
                // the only one that leaves (p mod len) unchanged for every p —
                // anything else moves the wrap point and rewrites the audio.
                // The loop base is therefore left alone.
                SCut *cut = tgt.cut;
                if( !cut ) return;   // an EVENT window: not this gesture's domain
                if( clipLoopSeg_ <= 0 ) {
                    // Same lazy capture as the right-edge loop gesture: an
                    // already-looping clip keeps its segment, a plain one starts
                    // looping the cut it currently shows.
                    length_t seg = clipLoopLen0_;
                    if( seg <= 0 ) {
                        length_t contentLen = cut->getContent().hasDuration()
                                              ? (length_t) cut->getContent().getDuration() : -1;
                        seg = lastClickDuration_;
                        if( contentLen >= 0
                            && seg > contentLen - (length_t) clipResizeOffset0_ )
                            seg = contentLen - (length_t) clipResizeOffset0_;
                    }
                    if( seg < SMV_CUT_MIN_TIME ) seg = SMV_CUT_MIN_TIME;
                    clipLoopSeg_ = seg;
                }
                offset_t end0 = clipDragStart0_ + (offset_t) lastClickDuration_;
                // Whole cycles away from the ORIGINAL start, rounded to nearest.
                length_t want = (length_t) clipDragStart0_
                              - (length_t) getTimeOf( ev->pos().x() );
                length_t k = ( want + ( want >= 0 ? clipLoopSeg_/2 : -clipLoopSeg_/2 ) )
                             / clipLoopSeg_;
                offset_t rStart = (offset_t)( (length_t) clipDragStart0_
                                              - k * clipLoopSeg_ );
                if( rStart < 0 ) rStart = 0;                     // no time before 0
                length_t newDur = (length_t) end0 - (length_t) rStart;
                if( newDur < SMV_CUT_MIN_TIME ) {
                    rStart = end0 - (offset_t) clipLoopSeg_;
                    if( rStart < 0 ) rStart = 0;
                    newDur = (length_t) end0 - (length_t) rStart;
                }
                if( newDur >= SMV_CUT_MIN_TIME
                    && ( rStart != lastClickSLink_->getStartTime()
                         || newDur != cut->getDuration()
                         || WarpedLen( clipLoopSeg_ ) != cut->getLoopLength() ) ) {
                    QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                    lastClickSLink_->setStartTime( rStart );
                    cut->setLoopLengthRaw( WarpedLen( clipLoopSeg_ ) );
                    cut->setDuration( newDur );
                    cut->queueWindowParamEvent( LOOP_LENGTH_CHANGE, (double) clipLoopSeg_ );
                    cut->queueWindowParamEvent( DURATION_CHANGE, (double) newDur );
                    cut->getPreviewCapture();  // Non-blocking: schedule async revalidation if needed
                    smv_.getModel()->getProject().notifyArrangementChanged();  // Cascade to live assets
                    update( oldRect );
                }
                update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
            } else if( lastClickedStart_ ) {
                // Drag the LEFT edge: move the clip start to the snapped mouse
                // time, trimming the front (cut start offset shifts with it).
                // TRIM IS WINDOW-GENERIC (proposal 42 M1): an EVENT clip is
                // resized through `SClipWindow`, which converts to its own
                // units exactly once, instead of being wrapped in an audio cut.
                SCut *cut = tgt.cut;
                if( !cut && !tgt.win ) return;
                offset_t end0 = clipDragStart0_ + (offset_t) lastClickDuration_;  // fixed right edge
                offset_t rStart = smv_.alignTime( getTimeOf( ev->pos().x() ) );
                // Keep at least the minimum length.
                if( (length_t) end0 - (length_t) rStart < SMV_CUT_MIN_TIME )
                    rStart = end0 - (offset_t) SMV_CUT_MIN_TIME;
                // The cut's start offset MAY go below zero: a clip can begin
                // before its data and render leading silence (proposal 23 — page
                // positions are signed now, so a negative source anchor no
                // longer wraps to ~1.8e19 and blank the page). Only the TIMELINE
                // start is pinned, since there is no time before zero.
                if( rStart < 0 ) rStart = 0;
                length_t shift = (length_t) rStart - (length_t) clipDragStart0_;
                if( (offset_t) rStart > end0 ) rStart = clipDragStart0_;   // safety
                shift = (length_t) rStart - (length_t) clipDragStart0_;
                offset_t rCutStart = (offset_t)( (length_t) clipResizeOffset0_ + shift );
                length_t rDur = (length_t) end0 - (length_t) rStart;
                // The duration is NOT clamped to the remaining content. This
                // edge owns the clip's START; the right edge is fixed at end0.
                // Clamping the length here dragged the far edge inward on any
                // clip longer than its data — trim 1 s off the front of a looping
                // 8 s clip and its end snapped from 8 s back to the 4 s of
                // sample behind it. Over-length clips are legitimate (see the
                // right-edge branch and "Remove loop").
                if( rDur >= SMV_CUT_MIN_TIME ) {
                    QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                    if( cut ) {
                    cut->setStartOffset( rCutStart );
                    cut->setDuration( rDur );
                    } else {
                        tgt.win->setWindowFromTimeline( rCutStart, rDur,
                                                        tgt.win->loopLength(),
                                                        tgt.win->stretchOrRate() );
                    }
                    lastClickSLink_->setStartTime( rStart );
                    if( cut ) cut->invalidateCapture();  // Drop cached render, schedule async revalidation
                    // Non-blocking: get preview cache (or stale) for live feedback during drag
                    if( cut ) cut->getPreviewCapture();
                    smv_.getModel()->getProject().notifyArrangementChanged();  // Cascade to live assets
                    update( oldRect );
                    update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
                }
            } else if( lastClickedEnd_ ) {
                // Drag the RIGHT edge: set the duration to the snapped mouse time.
                // Window-generic, like the left edge above (proposal 42 M1).
                // NOT clamped to the content length. A clip longer than its data
                // is a legitimate state, not an accident: a looping clip tiles
                // its segment for as long as you drag, and a plain one simply
                // runs into silence past the end of its sample — which is
                // exactly what "Remove loop" leaves behind, so that clip has to
                // survive a later extend too. The old clamp to
                // (contentLen - startOffset) snapped both back to about the
                // length of their data.
                SCut *cut = tgt.cut;
                if( !cut && !tgt.win ) return;
                offset_t rEnd = smv_.alignTime( getTimeOf( ev->pos().x() ) );
                length_t rDur = (length_t) rEnd - (length_t) clipDragStart0_;
                if( rDur < SMV_CUT_MIN_TIME ) rDur = SMV_CUT_MIN_TIME;
                QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                // Only rebuild if duration actually changed (not clamped to same value)
                if( rDur != ( cut ? cut->getDuration() : tgt.win->duration() ) ) {
                    if( cut ) {
                    cut->setDuration( rDur );
                    cut->queueWindowParamEvent( DURATION_CHANGE, (double) rDur );
                    cut->getPreviewCapture();  // Non-blocking: schedule async revalidation if needed
                    } else {
                        tgt.win->setDurationFromTimeline( rDur );
                    }
                    smv_.getModel()->getProject().notifyArrangementChanged();  // Cascade to live assets
                }
                update( oldRect );
                update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
            } else if( delta != 0 ) {
                // Move it.
                QRect oldVisibRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                offset_t oldStart = lastClickSLink_->getStartTime();
                if( newTrack ) {
                    lastClickSLink_->setParent(newTrack); // was: lastClickTrack_->removeChild( lastClickSLink_ ); newTrack->insertChild( lastClickSLink_ );
                    lastClickTrack_ = newTrack;
                    lastClickTrackIdx_ = newTrackIdx;
                }
                newStart = smv_.alignTime( newStart );
                if( newTrack || ((offset_t)newStart)!=oldStart ) {
                    lastClickSLink_->setStartTime( newStart );
                    // This means (currently), we move around a sample. 
                    // This is not nice
                    update( oldVisibRect );
                    if( newTrack ) {
                        QRect newVisibRect = getSLinkVisibRect( 
                            lastClickTrackIdx_, *lastClickSLink_ );
                        update( newVisibRect );
                    } else {
                        QRect newVisibRect = getSLinkVisibRect(
                            lastClickTrackIdx_, *lastClickSLink_ );
                        update( newVisibRect );
                    }
                }
                // For a group duplicate, drag the other copies with the anchor.
                if( clipDragIsDuplicate_ )
                    syncDuplicateGroup();
            }
        }
        // Note: cursor seeking on mouse movement is now deferred to mouseReleaseEvent
        // so clicks only set the cursor if no drag occurs.
    }
}

void SMVActualView::popupTrackMenu( STrack *t, const QPoint &globalPos )
{
    lastClickTrack_ = t;
    lastClickSLink_ = NULL;      // a head click names no clip
    ctGlobalShow();
    qGlobalPopup_->popup( globalPos );
}

void SMVActualView::contextMenuEvent( QContextMenuEvent *ev )
{
    // Range bar (ruler) gets its own menu.
    if( ev->pos().y() < SMV_TIME_RULER_HEIGHT ) {
        qRangeActClear_->setEnabled( rangeValid_ );
        qRangePopup_->popup( mapToGlobal( ev->pos() ) );
        return;
    }
    updateLastClickVars( ev->pos() );
    qGlobalPopup_->popup( mapToGlobal( ev->pos() ) );
}

// A left double-click in the empty area below the last track lane creates a new
// track — the timeline-canvas counterpart to the same gesture on the left
// track-control column (see SStdMixerView::eventFilter). A double-click that
// lands on an existing lane falls through to the base handler.

// ---------------------------------------------------------------------------
// W2 (proposal 28): warp-marker gestures. The marker STRIP is the top pixels
// of a clip rect; handles live at each anchor's warped position. Drag mutates
// live through SCut::setWarpAnchors; release reverts to the press snapshot
// and submits ONE undoable SMoveWarpMarkerAction (house revert-then-action).
// ---------------------------------------------------------------------------
static constexpr int kMarkerStripPx = 10;
static constexpr int kMarkerHitPx   = 5;

// W2.1: the window pin. A clip's startOffset is srcToWarped(srcStart) —
// MAP-DEPENDENT — so without an anchor at/below the clip start, every marker
// drag re-slopes the segment under srcStart and the whole displayed window
// slips (preview moves against the drag, other markers' screen positions
// shift, the dragged handle lags the mouse). Planting an identity anchor at
// the clip start pins the window; it is a normal visible marker (draggable,
// deletable) and audibly a no-op until moved. Returns true if a pin was
// submitted (the caller must re-read the anchor list).
static bool ensureStartPin( SCut *cut, SStdMixerView &smv,
                            STrack *track, SLink *link )
{
    const int64_t s0 = cut->getSrcStart().floorToInt();
    if( s0 <= 0 ) return false;                    // the origin already pins
    for( const twWarpAnchor &a : cut->getGrainParams().warpAnchors )
        if( a.src <= s0 ) return false;            // pinned at/below start
    const int64_t w0 = cut->getStartOffset().frames();
    if( w0 <= 0 ) return false;
    QList<int> path = strackpath::pathOf( smv.getModel(), track );
    path.append( track->indexOfChild( link ) );
    stimeline::submitActive(
        new SAddWarpMarkerAction( path, s0, w0 ) );
    return true;
}

bool SMVActualView::tryBeginMarkerDrag( QMouseEvent *ev )
{
    if( !lastClickSLink_ || !lastClickTrack_ ) return false;
    SCut *cut = dynamic_cast<SCut *>( &lastClickSLink_->getSObject() );
    if( !cut ) return false;
    const std::vector<twWarpAnchor> anchors = cut->getGrainParams().warpAnchors;
    if( anchors.empty() ) return false;

    const QRect vr = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
    if( !vr.isValid() || ev->pos().y() < vr.y()
        || ev->pos().y() >= vr.y() + kMarkerStripPx ) return false;

    const length_t dur = cut->getDurationBlocking();
    if( dur <= 0 || vr.width() <= 0 ) return false;
    const int64_t startOff = cut->getStartOffset().frames();

    // Sample-domain hit test on the VIRTUAL axis (same fix as the renderer:
    // pixel math against the clipped visibRect broke for clips wider than
    // the viewport). Tolerance = hit pixels converted to samples; looping
    // clips fold the click into the first repetition.
    const double t0 = (double) getTimeOf( vr.x() );
    const double t1 = (double) getTimeOf( vr.x() + vr.width() );
    double spp = ( t1 - t0 ) / (double) vr.width();
    if( spp <= 0.0 ) spp = 1.0;
    int64_t clickRel = (int64_t) getTimeOf( ev->pos().x() )
                     - (int64_t) lastClickSLink_->getStartTime();
    if( clickRel < 0 || clickRel >= (int64_t) dur ) return false;
    const bool    looping = cut->isLooping();
    const int64_t segLen  = looping ? cut->getLoopLength().frames()
                                    : (int64_t) dur;
    if( looping && segLen > 0 ) clickRel %= segLen;
    const int64_t tolSamples =
        (int64_t)( (double) kMarkerHitPx * spp ) + 1;

    for( const twWarpAnchor &a : anchors ) {
        const int64_t rel = a.warped - startOff;
        if( rel < 0 || rel >= segLen ) continue;
        if( std::llabs( clickRel - rel ) > tolSamples ) continue;

        QList<int> path = strackpath::pathOf( smv_.getModel(), lastClickTrack_ );
        path.append( lastClickTrack_->indexOfChild( lastClickSLink_ ) );
        if( hasPrimaryMod( ev->modifiers() ) ) {
            // Primary-modifier click on a handle (⌘ on macOS, Ctrl elsewhere):
            // delete the marker (undoable).
            stimeline::submitActive(
                new SDeleteWarpMarkerAction( path, a.src ) );
            return true;
        }
        // Pin the window before arming: without it the drag itself would
        // move startOffset and the whole clip display would slip.
        if( ensureStartPin( cut, smv_, lastClickTrack_, lastClickSLink_ ) )
            markerDragPre_ = cut->getGrainParams().warpAnchors;
        else
            markerDragPre_ = anchors;
        markerDragArmed_ = true;
        markerDragSrc_   = a.src;
        markerDragPreStartOffset_ = cut->getStartOffset().frames();
        return true;
    }
    return false;
}

void SMVActualView::updateMarkerDrag( QMouseEvent *ev )
{
    if( !lastClickSLink_ ) return;
    SCut *cut = dynamic_cast<SCut *>( &lastClickSLink_->getSObject() );
    if( !cut ) return;

    // Timeline position under the cursor, grid-snapped, made clip-relative,
    // then into the warped domain via the PRE-drag window (the drag itself
    // must not move its own reference frame). Looping clips fold the cursor
    // into the first repetition — same domain rule as add and hit-test.
    int64_t clipRel = (int64_t) smv_.alignTime( getTimeOf( ev->pos().x() ) )
                    - (int64_t) lastClickSLink_->getStartTime();
    if( cut->isLooping() ) {
        const int64_t segLen = cut->getLoopLength().frames();
        if( segLen > 0 && clipRel >= 0 ) clipRel %= segLen;
    }
    int64_t newWarped = clipRel + markerDragPreStartOffset_;

    // Clamp strictly between the neighbors (monotonicity by construction).
    int64_t lo = 0, hi = INT64_MAX;
    for( const twWarpAnchor &a : markerDragPre_ ) {
        if( a.src < markerDragSrc_ && a.warped + 1 > lo ) lo = a.warped + 1;
        if( a.src > markerDragSrc_ && a.warped - 1 < hi ) hi = a.warped - 1;
    }
    if( lo > hi ) return;
    if( newWarped < lo ) newWarped = lo;
    if( newWarped > hi ) newWarped = hi;

    std::vector<twWarpAnchor> edited = markerDragPre_;
    for( twWarpAnchor &a : edited )
        if( a.src == markerDragSrc_ ) { a.warped = newWarped; break; }
    cut->setWarpAnchors( edited );
    update();
}

void SMVActualView::finishMarkerDrag()
{
    markerDragArmed_ = false;
    if( !lastClickSLink_ || !lastClickTrack_ ) return;
    SCut *cut = dynamic_cast<SCut *>( &lastClickSLink_->getSObject() );
    if( !cut ) return;

    // Read the final dragged position, revert to the press snapshot, then
    // submit the ONE undoable action that re-applies it (its inverse then
    // captures the true pre-drag value).
    int64_t finalWarped = -1;
    for( const twWarpAnchor &a : cut->getGrainParams().warpAnchors )
        if( a.src == markerDragSrc_ ) { finalWarped = a.warped; break; }
    cut->setWarpAnchors( markerDragPre_ );
    if( finalWarped < 0 ) return;

    QList<int> path = strackpath::pathOf( smv_.getModel(), lastClickTrack_ );
    path.append( lastClickTrack_->indexOfChild( lastClickSLink_ ) );
    stimeline::submitActive(
        new SMoveWarpMarkerAction( path, markerDragSrc_, finalWarped ) );
    update();
}

bool SMVActualView::tryAddMarkerAt( QMouseEvent *ev )
{
    updateLastClickVars( ev->pos() );
    if( !lastClickSLink_ || !lastClickTrack_ ) return false;
    SCut *cut = dynamic_cast<SCut *>( &lastClickSLink_->getSObject() );
    if( !cut ) return false;
    const QRect vr = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
    if( !vr.isValid() || ev->pos().y() < vr.y()
        || ev->pos().y() >= vr.y() + kMarkerStripPx ) return false;

    int64_t clipRel = (int64_t) smv_.alignTime( getTimeOf( ev->pos().x() ) )
                    - (int64_t) lastClickSLink_->getStartTime();
    const length_t dur = cut->getDurationBlocking();
    if( clipRel < 0 || clipRel >= (int64_t) dur ) return false;
    // Looping clip: fold the click into the FIRST repetition — anchors live in
    // the loop segment's warped domain (the renderer tiles them back out; an
    // unfolded position lands past segLen, which is undrawable and inaudible).
    if( cut->isLooping() ) {
        const int64_t segLen = cut->getLoopLength().frames();
        if( segLen > 0 ) clipRel %= segLen;
    }
    const int64_t warped = clipRel + cut->getStartOffset().frames();
    // Identity add: the anchor pins the CURRENT mapping at this position, so
    // adding it changes nothing audibly until the user drags it.
    const int64_t src =
        cut->warpedToSourceExact( Fraction( warped ) ).floorToInt();
    if( src <= 0 ) return false;

    // First marker on this clip: pin the window start first (see
    // ensureStartPin) so later drags cannot slip the displayed window.
    ensureStartPin( cut, smv_, lastClickTrack_, lastClickSLink_ );

    QList<int> path = strackpath::pathOf( smv_.getModel(), lastClickTrack_ );
    path.append( lastClickTrack_->indexOfChild( lastClickSLink_ ) );
    stimeline::submitActive(
        new SAddWarpMarkerAction( path, src, warped ) );
    update();
    return true;
}

// The double-click "open this container" resolve. Every content object
// scutrndrinline.cpp's cutIsContainer() paints BLUE — a registered
// arrangement, a take stack, a plain folder-track window, or a nested
// container that is none of those — gets handled here, in the order the
// user asked for: a registered arrangement opens (or fronts) its tab; a
// take stack shows its take lanes; a folder track reveals its own lanes;
// anything else tries the tab route and, failing that, SAYS SO rather than
// doing nothing. Returns true iff it acted (opened, revealed, or reported a
// dead end) — the caller stops the double-click there either way; a false
// return means `link` was not a container at all and the caller should keep
// walking its other branches (the event-clip branch, in practice).
bool SMVActualView::tryOpenContainerClip( SLink *link )
{
    if( !link || !smv_.getModel() ) return false;
    SProject *proj = smv_.getModel()->getProjectSafe();
    if( !proj ) return false;

    SObject *raw = &link->getSObject();

    // Unwrap a take column at the LINK level: a take stack placed directly
    // on the track (SLink -> STakeStack, as opposed to an SCut whose
    // *content* is a take stack) resolves to its ACTIVE take first, so a
    // container cut sitting in a take column behaves exactly like the same
    // cut placed on its own.
    SObject *obj = raw;
    if( !SClipWindow::of( raw ) ) {
        // DIRECTLY-PLACED columns only — exactly what the comment above says.
        // Since proposal 42 M2 the seam forwards through a window, so an
        // unguarded `windowTakeAt(-1)` would unwrap a WRAPPED column to its
        // take as well; `content` would then be the wave and rule 2 below
        // ("the content is a take stack") would never fire, so double-clicking
        // a wrapped column would stop revealing its take lanes.
        if( SClipWindow *w = raw->windowTakeAt( -1 ) ) obj = &w->asObject();
    }

    if( SCut *cut = dynamic_cast<SCut *>( obj ) ) {
        SObject &content = cut->getContent();

        // Rule 1: a registered arrangement root -- open (or bring forward)
        // its tab. Unchanged from the pre-existing "asset clip" behaviour.
        const QString arrName = proj->arrangementNameOf( &content );
        if( !arrName.isEmpty() ) {
            if( SMainWindow *mw = findMainWindow() ) {
                if( SViewTabs *tabs = mw->ensureViewShell() )
                    tabs->openFor( &content, arrName );
            }
            return true;
        }

        // Rule 2: the content is a take stack (an SCut windowing a take
        // column) -- a take stack vends no detail editor at all, so "open
        // it" means SHOW ITS TAKE LANES on the clip's own track.
        if( dynamic_cast<STakeStack *>( &content ) )
            return revealTakeLanes( lastClickTrack_ );

        // Rule 3: the content is a plain track (what create-asset over a
        // FOLDER TRACK produces) -- reveal that track's own lanes. A track
        // with no children has nothing to reveal; fall through to rule 5.
        if( STrack *tr = dynamic_cast<STrack *>( &content ) ) {
            if( hasChildTracks( tr ) )
                return revealTrackLanes( tr );
        }

        // Rule 5: anything else cutIsContainer() paints blue (a nested
        // SCut, a container that vends no editor, an STrack with no
        // children) -- try the tab route, and if it refuses, SAY SO instead
        // of leaving the double-click looking like it did nothing.
        if( content.contentKind() == SContentKind::Audio
            && !content.getRandomSource() ) {
            if( SMainWindow *mw = findMainWindow() ) {
                if( SViewTabs *tabs = mw->ensureViewShell() ) {
                    if( tabs->openFor( &content, QString() ) ) return true;
                }
            }
            reportContainerDeadEnd( content );
            return true;
        }
        return false;
    }

    if( STakeStack *stack = dynamic_cast<STakeStack *>( raw ) ) {
        // `raw` IS the take stack (SLink -> STakeStack directly) and its
        // active take did not resolve to an SCut above -- either there is no
        // active take, or the active take is an EVENT window, which the
        // event-clip branch below already opens an editor for (contentKind()
        // is homogeneous across a stack's takes, so this one check covers
        // every take in it).
        if( stack->contentKind() != SContentKind::Event )
            return revealTakeLanes( lastClickTrack_ );
    }
    return false;
}

// Rule 2: show (never blindly toggle) a track's take lanes. A double-click
// that OPENS take lanes and a second double-click that leaves them open is
// acceptable; one that CLOSES lanes the user just opened is worse, so this
// only ever expands.
bool SMVActualView::revealTakeLanes( STrack *track )
{
    if( !track ) return false;
    const bool wasExpanded = smv_.isTrackTakesExpanded( track );
    if( !wasExpanded ) smv_.toggleTrackTakesExpanded( track );
    if( QMainWindow *mw = qobject_cast<QMainWindow *>( window() ) )
        mw->statusBar()->showMessage(
            wasExpanded
                ? QString( "Take lanes already shown for \"%1\"" ).arg( track->getSName() )
                : QString( "Take lanes shown for \"%1\"" ).arg( track->getSName() ),
            4000 );
    return true;
}

// Rule 3: reveal a folder track's own lanes -- expand every collapsed
// ancestor between the model root and `tr` (a track can be reachable by
// path yet have no row at all because a collapsed ancestor hides it), then
// `tr` itself so its children get a row.
bool SMVActualView::revealTrackLanes( STrack *tr )
{
    if( !tr ) return false;

    if( SObject *root = smv_.getModel() ) {
        SObject *cur = root;
        for( int idx : strackpath::pathOf( root, tr ) ) {
            if( STrack *anc = dynamic_cast<STrack *>( cur ) ) {
                if( anc->isCollapsed() ) smv_.toggleTrackCollapsed( anc );
            }
            SLink *lk = strackpath::childLinkAt( cur, idx );
            cur = lk ? &lk->getSObject() : nullptr;
            if( !cur ) break;
        }
    }
    if( tr->isCollapsed() ) smv_.toggleTrackCollapsed( tr );

    if( QMainWindow *mw = qobject_cast<QMainWindow *>( window() ) )
        mw->statusBar()->showMessage(
            QString( "Lanes shown for track \"%1\"" ).arg( tr->getSName() ), 4000 );
    return true;
}

// Rule 5's "say so": a blue clip whose content has no editor to open. Not
// silent -- a status message plus a TW_LOG line naming the content's kind
// and class, so a report of "double-click does nothing" can be told apart
// from an actual dead end.
void SMVActualView::reportContainerDeadEnd( SObject &content )
{
    const QString name = content.getSName().isEmpty()
        ? QString::fromLatin1( content.metaObject()->className() )
        : content.getSName();
    if( QMainWindow *mw = qobject_cast<QMainWindow *>( window() ) )
        mw->statusBar()->showMessage(
            QString( "\"%1\" has nothing to open here" ).arg( name ), 4000 );
    TW_LOGW( "ui.timeline",
             "double-click: container '%s' (class=%s, kind=%d) vends no editor",
             name.toUtf8().constData(),
             content.metaObject()->className(),
             (int) content.contentKind() );
}

void SMVActualView::mouseDoubleClickEvent( QMouseEvent *ev )
{
    // W2: double-click in a clip's marker strip adds a warp marker. LEFT
    // EXACTLY AS IS: the strip wins even over a container clip, so opening
    // one means double-clicking lower on its body.
    if( ev->button() == Qt::LeftButton && smv_.getModel()
        && tryAddMarkerAt( ev ) )
        return;

    // Double-click on a CONTAINER clip -- anything scutrndrinline.cpp's
    // cutIsContainer() paints BLUE -- resolves through tryOpenContainerClip()
    // above: a registered arrangement's tab, a take stack's lanes, a folder
    // track's lanes, or (as a last resort) a reported dead end. This
    // replaces the old "asset clip only" check, which left every other blue
    // clip a silent no-op.
    if( ev->button() == Qt::LeftButton && lastClickSLink_ && smv_.getModel() ) {
        if( tryOpenContainerClip( lastClickSLink_ ) ) {
            ev->accept();
            return;
        }
    }

    // Double-click on an EVENT (MIDI) clip opens the event editor for it.
    // tryAddMarkerAt() above already re-resolved lastClickSLink_/
    // lastClickTrack_ at this exact position (it calls updateLastClickVars()
    // itself), and Qt's own double-click delivery is press / release /
    // DBLCLICK / release - the leading press already selected the clip
    // (the arranger's plain single-click handler), so the editor opens onto
    // a clip that IS selected by the time it queries the selection.
    if( ev->button() == Qt::LeftButton && lastClickSLink_ && lastClickTrack_ ) {
        SObject *obj = &lastClickSLink_->getSObject();
        if( SClipWindow *w = obj->windowTakeAt( -1 ) ) obj = &w->asObject();
        if( obj->contentKind() == SContentKind::Event ) {
            QList<int> path = strackpath::pathOf( smv_.getModel(), lastClickTrack_ );
            path.append( lastClickTrack_->indexOfChild( lastClickSLink_ ) );
            if( SMainWindow *mw = findMainWindow() )
                mw->showEventEditor( strackpath::pathToString( path ) );
            ev->accept();
            return;
        }
    }

    // Double-click on a bare FOLDER LANE: a real row whose track has no clip
    // at this click position (lastClickSLink_ is null, so neither branch
    // above could have fired) but DOES have child tracks -- toggle its fold,
    // the same flip the head's fold triangle drives. This is the "I expected
    // the lanes to open" case (proposal 39's own dead end): a folder lane
    // with nothing of its own on it was otherwise unreachable from a
    // double-click at all.
    if( ev->button() == Qt::LeftButton && !lastClickSLink_ && lastClickTrack_
        && smv_.getModel() ) {
        const STrackRow *row = smv_.rowAt( lastClickTrackIdx_ );
        if( row && row->subKind == SubLaneKind::None && row->hasChildren ) {
            smv_.toggleTrackCollapsed( lastClickTrack_ );
            if( QMainWindow *mw = qobject_cast<QMainWindow *>( window() ) )
                mw->statusBar()->showMessage(
                    QString( "%1 \"%2\"" )
                        .arg( lastClickTrack_->isCollapsed() ? "Collapsed" : "Expanded" )
                        .arg( lastClickTrack_->getSName() ), 4000 );
            ev->accept();
            return;
        }
    }

    if( ev->button() == Qt::LeftButton && smv_.getModel() ) {
        if( ev->pos().y() >= SMV_TIME_RULER_HEIGHT
            && !smv_.rowAt( rowAtViewY( ev->pos().y() ) ) ) {
            smv_.ctAddTrackBelowLast();        // below the last visible lane
            return;
        }
    }
    QWidget::mouseDoubleClickEvent( ev );
}

void SMVActualView::mousePressEvent( QMouseEvent *ev )
{
    qWarning( "mousePressEvent() called button=%d.\n",
	      ev->button() );
    updateLastClickVars( ev->pos() );
    clipDragTakeIndex_ = -1;   // reset; the take-lane branch below may re-arm it

    // The top ruler band hosts the time-range selector.
    if( ev->pos().y() < SMV_TIME_RULER_HEIGHT ) {
        if( ev->button() == Qt::LeftButton ) {
            beginRangeDrag( ev->pos().x() );
            return;
        }
        if( ev->button() == Qt::RightButton ) {
            qRangeActClear_->setEnabled( rangeValid_ );
            qRangePopup_->popup( mapToGlobal( ev->pos() ) );
            return;
        }
    }

    // Automation sub-lanes and (while armed) clip envelopes own the press
    // before anything else does — proposal 37 P6. press() returns false for
    // every row and modifier it does not claim, so the clip gestures below are
    // untouched.
    if( ( ev->buttons() & Qt::LeftButton )
        && smv_.automationUi().press( *this, lastClickTrackIdx_, ev->pos(),
                                      ev->modifiers(), false ) ) {
        return;
    }

    // Take-lane rows: a left click on a take ACTIVATES it — the comping
    // gesture (proposal 17 phase 3, undoable select-take). Alt held on the
    // clip BODY instead arms a SLIP drag against the take actually under the
    // pointer, which need not be the active/comped one — SResizeClipAction's
    // `take` parameter already exists for exactly this ("the slip syncs to
    // the CORRESPONDING take", decision 3), only the gesture to reach it was
    // missing. Everything else (move/stretch/loop) is still unclaimed here.
    if( ev->buttons() & Qt::LeftButton ) {
        const STrackRow *clickRow = smv_.rowAt( lastClickTrackIdx_ );
        if( clickRow && clickRow->takeRow >= 0 ) {
            if( lastClickSLink_ ) {
                STakeStack *stack = takeStackOfLink( lastClickSLink_ );
                SClipWindow *take =
                    stack ? stack->takeAt( clickRow->takeRow ) : nullptr;
                if( stack && take ) {
                    // The click also selects the STACK's own link (never a
                    // single take — a stack is one clip on the timeline; only
                    // WHICH take sounds is per-lane), mirroring the composite
                    // lane's own-clip selection. Without this, whatever was
                    // selected before this click (or nothing) stayed the
                    // selection, so pressing 's' to split acted on that stale
                    // target instead of the column just clicked.
                    Qt::KeyboardModifiers modifiers = ev->modifiers();
                    switch( modifiers & Qt::ShiftModifier ) {
                    case Qt::ShiftModifier:
                        SApplication::app().submitToggleSelectionAction( lastClickSLink_ );
                        break;
                    default:
                        SApplication::app().submitSetSelectionAction( lastClickSLink_ );
                        break;
                    }
                    bool onBorder = lastClickedStart_ || lastClickedEnd_;
                    bool alt = modifiers & Qt::AltModifier;
                    SCut *takeCut = dynamic_cast<SCut*>( &take->asObject() );
                    if( takeCut && alt && !onBorder && lastClickLoopMarker_ == 0 ) {
                        clipDragArmed_        = true;
                        clipDragIsDuplicate_  = false;
                        clipDragIsLoopMarker_ = false;
                        clipDragIsSlip_       = true;
                        clipDragIsStretch_    = false;
                        clipDragIsLoop_       = false;
                        clipDragIsLoopStart_  = false;
                        clipDragTakeIndex_    = clickRow->takeRow;
                        clipDragTrack0_       = clickRow->track;
                        clipDragStart0_       = lastClickSLink_->getStartTime();
                        clipResizeOffset0_ = (offset_t) takeCut->getStartOffset().frames();
                        clipSrcStart0_     = takeCut->getSrcStart();
                        clipLoopLen0_      = takeCut->getLoopLength().frames();
                        clipStretch0_      = takeCut->getStretchExact();
                        // The LINK's own object's duration -- same
                        // reasoning as drawTakeLane's dur: the wrapping
                        // cut's own window governs the drag bounds on the
                        // legacy shape, and is identical to stack->
                        // getDuration() on the modern one.
                        lastClickDuration_ = lastClickSLink_->getSObject().getDuration();
                        update();
                        return;
                    }
                    // ARM THE SWIPE and decide at the RELEASE. A plain click
                    // still comps the WHOLE column (`select-take`, below); a
                    // drag comps the region it covers. Deciding here would
                    // need to know whether the pointer is going to move.
                    compSwipeArmed_ = true;
                    compSwipeTake_  = clickRow->takeRow;
                    compSwipeFrom_  = lastClickOffset_;
                    update();
                }
            }
            return;
        }
    }

    if( ev->buttons() & Qt::RightButton ) {
	// Also emulate legacy right mouse button events for the events
	// we do not receive via QContextHelpEvent
        qGlobalPopup_->popup( mapToGlobal( ev->pos() ) );
    } else if( ev->buttons() & Qt::LeftButton ) {
        // Detect, on which object we clicked.
        // We know the track,  so now calculate the time.
        if( lastClickTrack_ ) {
            // A lane click selects its track too, with the SAME
            // multi-selection semantics the track head already uses
            // (applyTrackSelectionClick): plain = select only this lane,
            // Ctrl = toggle, Shift = range-select from the last selected
            // track. Runs whether or not the click also landed on a clip.
            smv_.applyTrackSelectionClick( lastClickTrack_, ev->modifiers() );
            if( lastClickSLink_ ) {
                // From the EVENT, not QGuiApplication::keyboardModifiers().
                // The live-keyboard read was both less correct (it reports the
                // state now, not when the button went down) and untestable:
                // synthesized events carry their modifiers in the event, so the
                // whole modifier family — Ctrl-stretch, Alt-slip, Ctrl-duplicate
                // — was unreachable from drag-clip-edge.
                Qt::KeyboardModifiers modifiers = ev->modifiers();
                // W2: the marker strip outranks every clip-body gesture.
                if( tryBeginMarkerDrag( ev ) ) {
                    update();
                    return;
                }
                bool onBorder = lastClickedStart_ || lastClickedEnd_;
                if( hasPrimaryMod( modifiers ) && !onBorder
                    && lastClickLoopMarker_ == 0 ) {
                    // Primary-modifier click on a clip BODY (⌘ on macOS, Ctrl
                    // elsewhere): duplicate it and drag the live copy.
                    // (The primary modifier on a border means time-stretch —
                    // handled below.)
                    // If the clicked clip is part of a multi-selection, the whole
                    // selection is duplicated and dragged as a group (the clicked
                    // clip is the anchor; the rest follow by the same time/row
                    // delta). The release submits one undoable step per copy.
                    SLink *clicked = lastClickSLink_;
                    STrack *clickedTrack = lastClickTrack_;
                    SSelectionList group;
                    const SSelectionList &sel = SApplication::app().getSelectionList();
                    if( SApplication::app().isSLinkSelected( clicked ) && sel.size() > 1 )
                        group = sel;
                    else
                        group.append( clicked );

                    clipDupItems_.clear();
                    SProject *proj = &smv_.getModel()->getProject();
                    SLink *anchorCopy = NULL;
                    for( SLink *src : group ) {
                        STrack *st = dynamic_cast<STrack*>( src->parent() );
                        if( !st ) continue;
                        ClipDupItem it;
                        it.sourcePath = strackpath::pathOf( smv_.getModel(), st );
                        it.sourcePath.append( st->indexOfChild( src ) );
                        it.origStart = src->getStartTime();
                        it.origRow   = smv_.rowIndexOfTrack( st );
                        it.copy = makeDuplicateClip( proj, src->getSObject(), st,
                                                     src->getStartTime() );
                        if( !it.copy ) continue;
                        clipDupItems_.append( it );
                        if( src == clicked ) anchorCopy = it.copy;
                    }
                    if( !anchorCopy && !clipDupItems_.isEmpty() )
                        anchorCopy = clipDupItems_.first().copy;

                    if( anchorCopy ) {
                        // AC-a1: the app-wide SELECTION is deliberately left
                        // alone here — the ORIGINAL clip(s) stay selected/
                        // highlighted for the whole drag. anchorCopy is only a
                        // local bookkeeping pointer (syncDuplicateGroup and the
                        // release below read it), never pushed into
                        // SApplication's selection list. Doing that used to be
                        // how this gesture lost its own prior selection: the
                        // preview gets `delete`d before release submits
                        // anything, whose destroyed() signal auto-unselects it
                        // — so by the time the release's SSetSelectionAction
                        // snapshotted "the selection before this action" for
                        // its undo, the TRUE pre-drag selection (whatever the
                        // user had selected before Ctrl-pressing) was already
                        // gone, and one undo restored an EMPTY selection
                        // instead of it. Leaving the true selection untouched
                        // here is what makes the release's snapshot correct.
                        lastClickSLink_ = anchorCopy;
                        lastClickTrack_ = clickedTrack;
                        lastClickSelStartOffset_ = anchorCopy->getStartTime();
                        clipDragArmed_ = true;
                        clipDragIsDuplicate_ = true;
                        clipDragTrack0_ = clickedTrack;
                        clipDragStart0_ = anchorCopy->getStartTime();
                        clipDupAnchorStart_ = anchorCopy->getStartTime();
                        clipDupAnchorRow_ = smv_.rowIndexOfTrack( clickedTrack );
                        lastClickedStart_ = lastClickedEnd_ = false;   // move, not resize
                        update();
                    }
                } else {
                    lastClickSelStartOffset_ = lastClickSLink_->getStartTime();
                    // Arm a clip-edit drag (finalized as one undoable action on
                    // release). Which gesture: Alt on the body = slip; the primary
                    // modifier (⌘ on macOS, Ctrl elsewhere) on a border =
                    // time-stretch; right-edge upper half = loop; plain border =
                    // resize; plain body = move. Snapshot the full cut window so
                    // the release can revert-then-action.
                    bool alt = modifiers & Qt::AltModifier;
                    clipDragArmed_ = true;
                    clipDragIsDuplicate_ = false;
                    // A loop marker sits on the clip body; grabbing one outranks
                    // the body gestures (updateLastClickVars already cleared the
                    // edge flags for it).
                    // A FADE HANDLE outranks every body/edge gesture, like a
                    // loop marker (proposal 43 N5 UI).
                    clipDragIsFade_ = ( lastClickFadeHandle_ > 0 );
                    clipDragFadeWhich_ = lastClickFadeHandle_;
                    clipDragIsLoopMarker_ = ( !clipDragIsFade_
                                              && lastClickLoopMarker_ > 0 );
                    clipDragIsSlip_    = ( alt && !onBorder
                                           && !clipDragIsLoopMarker_
                                           && !clipDragIsFade_ );
                    clipDragIsStretch_ = ( hasPrimaryMod( modifiers ) && onBorder
                                           && !clipDragIsFade_ );
                    clipDragIsLoop_    = ( !hasPrimaryMod( modifiers )
                                           && lastClickedEnd_ && lastClickedEndUpper_ );
                    clipDragIsLoopStart_ = ( !hasPrimaryMod( modifiers )
                                             && lastClickedStart_ && lastClickedStartUpper_ );
                    clipLoopSeg_ = 0;   // captured lazily on the first loop move
                    clipDragTrack0_ = lastClickTrack_;
                    clipDragStart0_ = lastClickSLink_->getStartTime();
                    {
                        // Snapshot the window the gesture will EDIT, resolved
                        // by the same rule the drag and the release use
                        // (proposal 42 M1). The class-NAME test this replaced
                        // snapshotted ZEROS for a take column — so a border
                        // drag on one started from a slip of 0, a loop of 0 and
                        // a stretch of 1 whatever the column actually held.
                        const ClipEditTarget t0 =
                            clipEditTargetOf( lastClickSLink_ );
                        if( SCut *c0 = t0.cut ) clipFade0_ = c0->getFade();
                        else clipFade0_ = twClipFade();
                        if( t0.win ) {
                            clipResizeOffset0_ = t0.win->startOffset();
                            clipSrcStart0_     = t0.win->contentAnchorExact();
                            clipLoopLen0_      = t0.win->loopLength();
                            clipStretch0_      = t0.win->stretchOrRate();
                        } else {
                            clipResizeOffset0_ = 0;
                            clipSrcStart0_     = Fraction(0);
                            clipLoopLen0_      = 0;
                            clipStretch0_      = Fraction(1);
                        }
                    }
                    switch( modifiers & (Qt::ShiftModifier) ) {
                    case Qt::ShiftModifier: // Shift: toggle this object in the selection.
                        SApplication::app().submitToggleSelectionAction( lastClickSLink_ );
                        break;
                    default: // No modifier, new one becomes selected.
                        SApplication::app().submitSetSelectionAction( lastClickSLink_ );
                        break;
                    }
                    // FIXME: Only update the object itselves.
                    update();
                }
            }
        }
        // Note: cursor seeking is now deferred to mouseReleaseEvent so it only
        // happens on a click (no drag).
    }
}

void SStdMixerView::setTimeGridSpec( const STimeGridSpec &newSpec )
{
    timeGridSpec_ = newSpec;
    emit timeGridSpecChanged( timeGridSpec_ );
}

/**
 * Return the absolute time (in samples, for now) of the given x position.
 * This depends on the zoom factor of this model.
 */
offset_t SMVActualView::InlineRenderContext::getTimeOf( int x ) const
{
    return getMixerView().getTimeOf( x );
}

/**
 * When the duration of our content has changed, we have to resize our scroller.
 */
void SStdMixerView::contentDurationChanged( length_t newDur )
{    
    qWarning( "contentDurationChanged: %d:%d.\n", 
	      (int)(newDur>>32),
	      (int)newDur );    
#if 0
    if( newDur > 0x7fffffff ) {
        qWarning( "Clipping content duration to INT_MAX.\n" );
        newDur = 0x7fffffff;
    }
    int currValue = qScrollHoriz_->value();
    int pageStep = qScrollHoriz_->pageStep();
    if( currValue+pageStep > newDur ) {
        currValue = newDur-pageStep;
        if( currValue<0 ) currValue = 0;
        timeSliderMoved( currValue );
    }
    qWarning( "Setting maxValue to %d.\n", qMax( 0, (int) newDur - pageStep ) );
    qScrollHoriz_->setMaxValue( qMax( 0, (int)newDur-pageStep ) );
#else
    recalcPageStep();
#endif
}

// The take-lane row count of a track: the widest take stack among its clips
// (proposal 17 phase 3). 0 = no stacks, nothing to expand.
static int maxTakesOf( STrack *tk )
{
    int maxTakes = 0;
    for( SLink *lk : tk->childLinks() ) {
        if( STakeStack *stack = takeStackOfLink( lk ) ) {
            if( stack->nTakes() > maxTakes ) maxTakes = stack->nTakes();
        }
    }
    return maxTakes;
}

void SStdMixerView::appendRowsFor( SObject *container, int depth )
{
    for( SLink *lk : container->childLinks() ) {
        STrack *tk = dynamic_cast<STrack*>( &lk->getSObject() );
        if( !tk ) continue;          // clips render inside their track's own lane
        // STrack::hasChildTracks(), not a local copy: the folder-sum overlay
        // asks the same question (proposal 39 M3.1) and two spellings of "is
        // this a folder" is one more than there should be.
        bool kids = tk->hasChildTracks();
        bool col = tk->isCollapsed();
        rows_.append( STrackRow{ tk, lk, container, depth, kids, col } );
        // Take lanes directly below the track's composite lane.
        if( takesExpanded_.contains( tk ) ) {
            const int mt = maxTakesOf( tk );
            for( int k = 0; k < mt; ++k ) {
                rows_.append( STrackRow{ tk, lk, container, depth,
                                         false, false, k, SubLaneKind::Take } );
            }
        }
        // ...then this track's automation lanes, under the same sub-lane rule
        // (proposal 37 P6): no head of their own, covered by the track's.
        appendAutomationRowsFor( tk, lk, container, depth );
        if( kids && !col ) appendRowsFor( tk, depth+1 );   // recurse if expanded
    }
}

void SStdMixerView::toggleTrackTakesExpanded( STrack *t )
{
    if( !t ) return;
    if( takesExpanded_.contains( t ) ) takesExpanded_.remove( t );
    else                               takesExpanded_.insert( t );
    refreshTrackTree();
}

// Applied actions (add-take, remove-take, split of a stack …) can change an
// expanded track's take-row count without any track-structure signal. Rebuild
// the rows; only a changed count needs the full (control column) refresh.
void SStdMixerView::onArrangementChangedRows()
{
    if( takesExpanded_.isEmpty() ) return;   // canvas repaint happens anyway
    const int before = rows_.size();
    // Capture the scroll FRACTION before the rebuild (fix/arranger-ui-fixes C
    // item 7): scroll is pixel-granular now, and a row boundary does not
    // survive a row-count change intact, so re-anchor by fraction of the
    // total height rather than by row.
    const int oldTotal = totalRowsHeight();
    const double frac = ( oldTotal > 0 ) ? (double) qContent_->getUpperLeftY() / (double) oldTotal : 0.0;
    rebuildRows();
    if( rows_.size() != before ) {
        rebuildControlColumn();
        nTracksChanged();
        reanchorScrollByFraction( frac );
    } else {
        // Same lane count, but a take could have moved between tracks — the
        // heads are cheap to re-place and must not be left behind.
        layoutControlColumn();
    }
    qContent_->update();
}

QString SStdMixerView::rootName() const
{
    if( !model_ ) return QString();
    SProject *proj = model_->getProjectSafe();
    return proj ? proj->arrangementNameOf( model_ ) : QString();
}

void SStdMixerView::rebuildRows()
{
    pruneUiState();          // one walk, every per-track UI-state set (P6)
    rows_.clear();
    if( model_ ) appendRowsFor( model_, 0 );
    rebuildRowGeometry();
}

// --- row geometry -------------------------------------------------------
// Lane heights are per-row and must be treated as arbitrary: a track carries
// its own height scale, and one track can own several lanes (take lanes
// today, automation lanes later). The only supported way from a row index to
// a pixel is rowTop()/rowHeight(); the only way back is rowAtLaneY().

double SStdMixerView::trackHeightScale( const STrack *t ) const
{
    return t ? trackScale_.value( t, 1.0 ) : 1.0;
}

void SStdMixerView::setTrackHeightScale( STrack *t, double scale )
{
    if( !t ) return;
    scale = qBound( LANE_SCALE_MIN, scale, LANE_SCALE_MAX );
    if( qFuzzyCompare( trackHeightScale( t ), scale ) ) return;
    if( qFuzzyCompare( scale, 1.0 ) ) trackScale_.remove( t );
    else                              trackScale_.insert( t, scale );
    // Heights changed under the scroll anchor: re-derive the geometry, then
    // re-anchor the scroll by FRACTION of the total height (fix/arranger-ui-
    // fixes C item 7) rather than by row — pixel scroll does not survive a
    // row's own height changing size while it holds the anchor.
    const int oldTotal = totalRowsHeight();
    const double frac = ( oldTotal > 0 ) ? (double) qContent_->getUpperLeftY() / (double) oldTotal : 0.0;
    rebuildRowGeometry();
    reanchorScrollByFraction( frac );
    recalcPageStep();
    layoutControlColumn();
    qContent_->update();
}

// Height of one lane. A sub-lane may be scaled differently from the track
// lane it hangs off — today it matches (SUB_LANE_SCALE = 1.0), and changing
// that is a one-line edit because nothing else assumes the two are equal.
int SStdMixerView::rowHeightOf( const STrackRow &row ) const
{
    static const double SUB_LANE_SCALE = 1.0;
    double s = trackHeightScale( row.track );
    if( row.isSubLane() ) s *= SUB_LANE_SCALE;
    int h = (int) ( getTrackHeight() * s + 0.5 );
    return qMax( 6, h );      // same floor as SMVActualView::setTrackHeight
}

void SStdMixerView::rebuildRowGeometry()
{
    rowTop_.resize( rows_.size()+1 );
    int y = 0;
    for( int i=0; i<rows_.size(); ++i ) {
        rowTop_[i] = y;
        rows_[i].height = rowHeightOf( rows_.at( i ) );
        y += rows_.at( i ).height;
    }
    rowTop_[rows_.size()] = y;          // total height, one call away
}

// See the header for the rationale (fix/arranger-ui-fixes C item 7).
void SStdMixerView::reanchorScrollByFraction( double frac )
{
    const int total = totalRowsHeight();
    int y = (int)( frac * total + 0.5 );
    if( y < 0 ) y = 0;
    if( total > 0 && y >= total ) y = total - 1;
    const int row = rowAtLaneY( y );
    qContent_->setTopPixel( row >= 0 ? rowTop( row ) : 0 );
}

int SStdMixerView::rowTop( int row ) const
{
    if( rowTop_.isEmpty() ) return 0;
    if( row < 0 ) return 0;
    if( row >= rowTop_.size() ) return rowTop_.last();
    return rowTop_.at( row );
}

int SStdMixerView::rowHeight( int row ) const
{
    if( row < 0 || row >= rows_.size() ) return getTrackHeight();
    return rows_.at( row ).height;
}

int SStdMixerView::rowAtLaneY( int y ) const
{
    if( y < 0 || rows_.isEmpty() || y >= totalRowsHeight() ) return -1;
    // Binary search over the prefix sums: the row whose span contains y.
    int lo = 0, hi = rows_.size()-1;
    while( lo < hi ) {
        int mid = (lo+hi+1)/2;
        if( rowTop( mid ) <= y ) lo = mid; else hi = mid-1;
    }
    return lo;
}

// The lane group of a track lane: the lane itself plus every sub-lane hanging
// off it. One head covers the whole group, so a track that owns several lanes
// gets one strip spanning all of them instead of a strip and a gap.
int SStdMixerView::laneGroupHeight( int row ) const
{
    if( row < 0 || row >= rows_.size() ) return getTrackHeight();
    int h = rowHeight( row );
    for( int i=row+1; i<rows_.size(); ++i ) {
        if( !rows_.at( i ).isSubLane() || rows_.at( i ).track != rows_.at( row ).track )
            break;
        h += rowHeight( i );
    }
    return h;
}

// The control column shares the canvas' vertical origin — same ruler band,
// same scroll offset — which is exactly why the heads track the lanes.
int SStdMixerView::controlYOfRow( int row ) const
{
    return qContent_->laneTop( row );
}

int SStdMixerView::rowAtControlY( int y ) const
{
    return qContent_->rowAtViewY( y );
}

// THE place head geometry is decided. The box itself never moves (its layout
// owns it); the heads inside carry the scroll and are clipped by it.
// See layoutControlColumn()'s comment for what this row IS. Shared with
// tkCheckLaneAlignment() (fix/arranger-ui-fixes C item 5) so a head's real
// clamped geometry and the testkit's idea of "correct" geometry cannot
// silently compute two different rows.
int SStdMixerView::rulerStraddleHeadRow() const
{
    int headRow = 0;
    if( !rows_.isEmpty() ) {
        headRow = qBound( 0, (int) qContent_->getTopRow(), rows_.size()-1 );
        while( headRow > 0 && rows_.at( headRow ).isSubLane() ) --headRow;
    }
    return headRow;
}

// Shared by layoutControlColumn() and tkCheckLaneAlignment() (fix/arranger-
// ui-fixes C item 5), so the real head geometry and the testkit's expected
// geometry cannot diverge. Clamps a head's [top,height) so it never draws
// above SMV_TIME_RULER_HEIGHT — but only for `isHeadRow` true, the ONE row
// whose lane GROUP contains the current scroll offset. Every row strictly
// before it already ends at or before the ruler line (adjacent prefix-sum
// geometry: row r's bottom edge is always exactly row r+1's top edge, so the
// row just above the scrolled-to one always butts up against it — true
// before this feature too, and left alone). Every row strictly after it
// starts at or after the ruler line. Only the scrolled-to row can straddle
// it, and only now that scroll is pixel-granular (a row-granular scroll
// always landed it exactly AT the ruler line).
static void clampHeadToRulerBand( bool isHeadRow, int &top, int &h )
{
    if( isHeadRow && top < SMV_TIME_RULER_HEIGHT ) {
        h -= ( SMV_TIME_RULER_HEIGHT - top );
        top = SMV_TIME_RULER_HEIGHT;
        if( h < 0 ) h = 0;
    }
}

void SStdMixerView::layoutControlColumn()
{
    if( !qTrackControlBox_ || !controlArray_ ) return;
    const int w = trackControlWidth_;
    const int headRow = rulerStraddleHeadRow();
    for( int c=0; c<controlArray_->size(); ++c ) {
        SSMVMixerControl *mc = controlArray_->at( c );
        if( !mc ) continue;
        const int row = ( c < controlRow_.size() ) ? controlRow_.at( c ) : c;
        int top = controlYOfRow( row );
        int h = laneGroupHeight( row );
        clampHeadToRulerBand( row == headRow, top, h );
        mc->setGeometry( 0, top, w, h );
    }
    if( dropIndicator_ ) dropIndicator_->raise();
}

const STrackRow *SStdMixerView::rowAt( int i ) const
{
    if( i<0 || i>=rows_.size() ) return NULL;
    return &rows_.at( i );
}

int SStdMixerView::rowIndexOfTrack( const STrack *t ) const
{
    for( int i=0; i<rows_.size(); ++i ) {
        if( rows_.at( i ).track == t ) return i;
    }
    return -1;
}

void SStdMixerView::toggleTrackCollapsed( STrack *t )
{
    if( !t ) return;
    // Fold state lives on the track itself now (fix/track-list-polish m), so
    // this call is a straight flip-and-rebuild rather than a set membership
    // toggle — and it is what makes the flip visible on the next save.
    t->setCollapsed( !t->isCollapsed() );
    refreshTrackTree();
}

// Recreate the control strips so there is exactly one per visible lane, indented
// to its depth. (Cheap — there are only a handful of tracks — and it keeps the
// control column in lockstep with rows_ for every structural change.)
void SStdMixerView::rebuildControlColumn()
{
    // Defer destruction: a rebuild can be triggered from *inside* a control's
    // own mouse handler (a grip-drag release that reparents, or a fold-triangle
    // click), so deleting the control synchronously would free it while Qt is
    // still dispatching its event -> use-after-free. hide() + deleteLater() lets
    // the handler unwind first.
    for( SSMVMixerControl *mc : *controlArray_ ) {
        if( mc ) { mc->hide(); mc->deleteLater(); }
    }
    controlArray_->clear();
    controlRow_.clear();
    for( int i=0; i<rows_.size(); ++i ) {
        const STrackRow &row = rows_.at( i );
        // Sub-lanes (take lanes, later automation) carry no channel strip of
        // their own — the track's head spans them (laneGroupHeight). So the
        // controls are NOT index-parallel with rows_: remember each head's row.
        if( row.isSubLane() ) continue;
        SSMVMixerControl *mc = new SSMVMixerControl( qTrackControlBox_, *this, *row.track );
        mc->setTreeInfo( row.depth, row.hasChildren, row.collapsed );
        mc->show();
        controlArray_->append( mc );
        controlRow_.append( i );
    }
    // The box is a viewport owned by the holder's layout — never moved or
    // resized here. Only the heads inside it are placed.
    layoutControlColumn();
}

// Single entry point for any structural change (add/remove/reorder/group/fold):
// rebuild the flattened tree, the control column, the scroll range, and repaint.
void SStdMixerView::refreshTrackTree()
{
    // Capture the scroll FRACTION before the rebuild (fix/arranger-ui-fixes C
    // item 7): a structural change (add/remove/reorder/fold) can change every
    // row's height and index, so "the same row" no longer names a stable
    // position the way a fraction of the whole does.
    const int oldTotal = totalRowsHeight();
    const double frac = ( oldTotal > 0 ) ? (double) qContent_->getUpperLeftY() / (double) oldTotal : 0.0;
    rebuildRows();
    rebuildControlColumn();
    nTracksChanged();
    reanchorScrollByFraction( frac );
    qContent_->update();
}

// The model signals still arrive incrementally; a full refresh is simplest and
// correct (nesting changes do not map cleanly onto add/remove-at-index).
void SStdMixerView::addMixerControl( int, STrack & )    { refreshTrackTree(); }
void SStdMixerView::removeMixerControl( int, STrack & ) { refreshTrackTree(); }
void SStdMixerView::tracksReordered()                   { refreshTrackTree(); }

// Map a Y in the control-column to an insertion gap 0..n among the visible
// lanes: the number of lanes whose midpoint the pointer is past.
int SStdMixerView::insertSlotAt( int y ) const
{
    int slot = 0;
    for( int i=0; i<rows_.size(); ++i ) {
        if( y > controlYOfRow( i ) + rowHeight( i )/2 ) ++slot;
    }
    return slot;
}

void SStdMixerView::beginTrackDrag( SSMVMixerControl *control )
{
    dragControl_ = control;
    if( dropIndicator_ ) dropIndicator_->raise();
}

void SStdMixerView::resolveDrop( int y, STrack **onto, int *topSlot ) const
{
    *onto = NULL;
    int n = rowCount();
    int r = rowAtControlY( y );
    if( r>=0 && r<n ) {
        int h = rowHeight( r );
        int within = y - controlYOfRow( r );
        const STrackRow *row = rowAt( r );
        // Over the middle half of a lane -> nest onto that track.
        if( row && within > h/4 && within < (3*h)/4 ) *onto = row->track;
    }
    // Insertion gap among top-level lanes = how many sit above the drop.
    int slot = 0;
    for( int i=0; i<n; ++i ) {
        if( rowAt( i )->depth != 0 ) continue;
        if( y > controlYOfRow( i ) + rowHeight( i )/2 ) ++slot;
    }
    *topSlot = slot;
}

void SStdMixerView::updateTrackDrag( int yInControlBox )
{
    if( !dragControl_ || !dropIndicator_ ) return;
    STrack *onto = NULL; int slot = 0;
    resolveDrop( yInControlBox, &onto, &slot );
    // Never offer a nest into a lane that is itself part of the drag (or below
    // one) — endTrackDrag would refuse it, and the outline would be a lie.
    if( onto ) {
        for( STrack *t : pruneNestedTargets(
                 selectionTargets( &dragControl_->getTrack() ) ) ) {
            if( strackpath::isSelfOrDescendant( onto, t ) ) { onto = NULL; break; }
        }
    }
    if( onto ) {
        // Nest: outline the whole target lane group.
        int r = rowIndexOfTrack( onto );
        dropIndicator_->setStyleSheet( "border:2px solid #2080ff; background:transparent;" );
        dropIndicator_->setGeometry( 0, controlYOfRow( r ), trackControlWidth_,
                                     laneGroupHeight( r ) );
    } else {
        // Between: a thin insertion line at the nearest lane boundary.
        dropIndicator_->setStyleSheet( "background:#2080ff; border:none;" );
        int yLine = controlYOfRow( insertSlotAt( yInControlBox ) );
        if( yLine>0 ) yLine -= 1;
        dropIndicator_->setGeometry( 0, yLine, trackControlWidth_, 3 );
    }
    dropIndicator_->show();
    dropIndicator_->raise();
}

void SStdMixerView::endTrackDrag( int yInControlBox )
{
    if( dropIndicator_ ) dropIndicator_->hide();
    SSMVMixerControl *control = dragControl_;
    dragControl_ = NULL;
    if( !control || !model_ ) return;
    STrack *dragged = &control->getTrack();

    // Drag the whole selection when the grip that was grabbed belongs to it —
    // outermost tracks only, since a folder carries its subtree along.
    const QList<STrack*> targets =
        pruneNestedTargets( selectionTargets( dragged ) );
    if( targets.isEmpty() ) return;

    STrack *onto = NULL; int slot = 0;
    resolveDrop( yInControlBox, &onto, &slot );

    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;

    // Dropped onto a lane:
    if( onto ) {
        // A drop onto a lane that is itself being dragged (or into one of the
        // dragged subtrees) has no meaning — refuse the whole gesture rather
        // than let part of it through.
        for( STrack *t : targets )
            if( strackpath::isSelfOrDescendant( onto, t ) ) return;

        if( macro ) stack->beginMacro( QStringLiteral( "Move tracks" ) );
        for( STrack *t : targets ) {
            int ri = rowIndexOfTrack( t );
            SObject *curParent = (ri>=0) ? rowAt( ri )->parent : NULL;
            // Already inside the target folder -> no-op (don't submit a doomed
            // reparent; SReparentTrackAction refuses a same-container move).
            if( (SObject*)onto == curParent ) continue;
            // Otherwise nest under it (the action also guards cycles).
            stimeline::submitActive( new SReparentTrackAction(
                strackpath::pathOf( model_, t ),
                strackpath::pathOf( model_, onto ), -1 ) );
        }
        if( macro ) stack->endMacro();
        return;
    }

    // Dropped on a boundary -> reorder at top level, or pop a nested track out.
    // `slot` is an insertion GAP among the top-level lanes, and it walks: each
    // track that lands takes the gap and the next one goes after it, so a
    // dragged block keeps its internal order.
    if( macro ) stack->beginMacro( QStringLiteral( "Move tracks" ) );
    for( STrack *t : targets ) {
        int nTop = model_->getNTracks();
        int fromTop = model_->indexOfChildObject( *t );
        if( fromTop>=0 ) {
            int target = (slot>fromTop) ? slot-1 : slot;
            if( target<0 ) target = 0;
            if( target>=nTop ) target = nTop-1;
            if( target!=fromTop )
                stimeline::submitActive(
                    new SMoveTrackAction( QList<int>{ fromTop }, target ) );
            slot = target + 1;      // the next one goes just below this one
        } else {
            int target = slot;
            if( target<0 ) target = 0;
            if( target>nTop ) target = nTop;
            stimeline::submitActive( new SReparentTrackAction(
                strackpath::pathOf( model_, t ), QList<int>{}, target ) );
            slot = target + 1;
        }
    }
    if( macro ) stack->endMacro();
}

void SStdMixerView::nTracksChanged()
{
    // Pixel-granular (fix/arranger-ui-fixes C): pageStep is a PIXEL count now
    // (set by recalcPageStep(), which a resize/zoom already keeps current),
    // so "does the current scroll position still fit" compares pixels to
    // pixels — totalRowsHeight(), not rowCount().
    const int pageStep = qScrollVert_->pageStep();
    const int total = totalRowsHeight();
    int currValue = qScrollVert_->value();
    if( currValue+pageStep > total ) {
        currValue = qMax( 0, total-pageStep );
        trackSliderMoved( currValue );
    }
    qScrollVert_->setMaximum( verticalScrollMaximum( pageStep ) );
}

void SStdMixerView::avLeftOffsetChanged( offset_t newValue )
{
    // The bar's domain is FRAMES now (fix/arranger-ui-fixes B), matching
    // getLeftOffset() exactly — no HSliderRange rescale, so no quantisation
    // and no "+dur" fudge that used to map offset 0 to slider value 1.
    // Recompute the horizontal extent/range FIRST: an unbounded wheel-pan
    // past the last clip must grow the bar's maximum before the bar's VALUE
    // is synced to it, or the bar would visibly lag one step behind what the
    // wheel can already reach.
    recalcPageStep();
    offset_t v = newValue;
    if( v < 0 ) v = 0;
    if( v > 0x7fffffff ) v = 0x7fffffff;
    // Re-entrancy guard: this slot exists only to keep the SCROLLBAR in step
    // with the canvas' exact frame offset. setValue() re-fires the bar's own
    // valueChanged() -> timeSliderMoved() -> setLeftOffset(); blocking it here
    // means the wheel's exact offset is set once, not round-tripped through
    // the bar and back.
    if( (int) v != qScrollHoriz_->value() ) {
        QSignalBlocker blocker( qScrollHoriz_ );
        qScrollHoriz_->setValue( (int) v );
    }
}

void SStdMixerView::timeSliderMoved( int newValue )
{
    if( newValue<0 ) newValue = 0;
    // The bar's domain is FRAMES (fix/arranger-ui-fixes B) — a drag lands
    // EXACTLY where the thumb is, at any zoom or duration, with no rescale.
    qContent_->setLeftOffset( (offset_t) newValue );
}

// AC-g1: see the header comment on why this listens to actionTriggered rather
// than valueChanged. `action` itself is not consulted — every user-driven
// trigger (drag, page step, arrow click) counts as "the user scrolled by
// hand", and the hold does not distinguish among them.
void SStdMixerView::onHScrollUserAction( int /*action*/ )
{
    if( qContent_ ) qContent_->armFollowHold();
}

void SStdMixerView::trackSliderMoved( int newValue )
{
    if( newValue<0 ) {
	   qWarning( "SStdMixerView::trackSliderMoved(): newValue was less than zero." );
	   newValue = 0;
    }
    // The bar's domain is PIXELS (fix/arranger-ui-fixes C).
    qContent_->setTopPixel( newValue );
}

/**
 * Ensure, that the link passed points to an SCut object.
 * If it is not, a new SCut object referencing the object passed
 * is created. The new object is inserted at the same point
 * in the parent.
 *
 * The passed link is REPLACED (and deleted): the cut creates its own content
 * link. The old adopting-ctor variant took `lk` as the cut's content_ and the
 * `delete lk` below then left content_ dangling (use-after-free). The delete
 * happens only after the cut holds its own ref, so the content's refcount
 * never touches zero (removeRef()'s deleteLater() cannot be rescinded).
 */
// Drive one clip-edge gesture through the arranger's real mouse handlers. See
// the header for why this exists: every clip-edge clamp lives in the drag code,
// and nothing else in the qxa suite can reach it.
// WHERE A SYNTHESIZED GESTURE GRABS A CLIP. Factored out of dragClipEdge so
// the HOVER verb lands on exactly the same point a drag would (proposal 43
// N5 UI's rule, and proposal 41 M7's before it: one geometry, or a
// synthesized gesture starts testing a box a hand never touches).
// `dropTime` only matters for the canvas grow below; pass the grab time for
// a hover.
bool SStdMixerView::grabPointFor( int rowIdx, int clipIdx, int grabWhere,
                                  offset_t dropTime, bool upperHalf,
                                  QPoint &outGrab, QPoint &outDrop )
{
    const STrackRow *row = rowAt( rowIdx );
    if( !row || !row->track || !qContent_ ) return false;

    // Nested tracks appear in a folder's child list but are lanes, not clips.
    SLink *clip = NULL;
    int n = 0;
    for( SLink *lk : row->track->childLinks() ) {
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) continue;
        if( n++ == clipIdx ) { clip = lk; break; }
    }
    if( !clip || !clip->hasStartTime() || !clip->getSObject().hasDuration() )
        return false;

    offset_t start = clip->getStartTime();
    length_t dur   = clip->getSObject().getDuration();
    // Land inside the edge grab band: [startX, startX+LEFT) or [endX-RIGHT, endX);
    // GrabBody lands at the clip's midpoint, clear of both bands, which is the
    // only place the body gestures (slip / duplicate / move) can arm.
    int sx = qContent_->getXPosOfOffset( start );
    int ex = qContent_->getXPosOfOffset( start + (offset_t) dur );
    int th = rowHeight( rowIdx );
    int laneTop = qContent_->laneTop( rowIdx );

    int x0, y;
    if( grabWhere == GrabFadeIn || grabWhere == GrabFadeOut ) {
        // proposal 43 N5 UI: land inside the clip's own FADE handle, at the
        // SAME geometry the renderer draws it with and `fadeHandleAt` grabs
        // it by -- one function, so a synthesized gesture cannot start testing
        // a box the user's hand would miss.
        SCut *cut = dynamic_cast<SCut*>( &clip->getSObject() );
        if( !cut ) return false;
        const twClipFade fade = cut->getFade();
        QRect clipRect( sx, qContent_->laneTop( rowIdx )+2, ex-sx, th-4 );
        const bool isOut = ( grabWhere == GrabFadeOut );
        const int trueX = isOut
            ? qContent_->getXPosOfOffset( start + (offset_t)( dur - fade.outLen ) )
            : qContent_->getXPosOfOffset( start + (offset_t) fade.inLen );
        const int hx = scutFadeHandleX( clipRect, trueX, isOut,
                                        SCUT_FADE_EDGE_BAND_PX );
        if( hx < 0 ) return false;
        const QRect box = scutFadeHandleRect( clipRect, hx );
        if( box.isEmpty() ) return false;
        x0 = box.center().x();
        y  = box.center().y();
    } else if( grabWhere == GrabTag ) {
        // proposal 41 D15/M7: land inside THIS clip's own tag chip -- the
        // SAME geometry tagChipRect()/tagHitTestAt() use, computed from the
        // clip's REAL start/duration rather than an approximation of it.
        // `vr` mirrors the inset content rect strackrndrinline.cpp's draw()
        // computes right before it asks for the chip.
        QRect vr( sx + 1, laneTop + 1, ( ex - sx ) - 2, th - 2 );
        if( vr.left() < 0 ) vr.setLeft( 0 );
        if( vr.right() > qContent_->width() - 1 ) vr.setRight( qContent_->width() - 1 );
        const QRect chip = STrackRendererInline::tagChipRect( clip->getSObject(), vr );
        if( chip.isEmpty() ) return false;   // nothing to grab -- lane too short/narrow
        // Near the chip's RIGHT edge: far enough from vr.left() to clear the
        // GrabStart trim band (SMV_LEFT_DRAG_PIXEL) whenever the chip is wide
        // enough to have room for it, so a plain move gesture arms rather
        // than a resize -- and still inside the clip's declared chip
        // footprint, whatever got painted over part of it since.
        x0 = chip.right();
        y  = chip.center().y();
    } else {
        x0 = ( grabWhere == GrabEnd )  ? ex - 1
           : ( grabWhere == GrabBody ) ? ( sx + ex ) / 2
                                       : sx;
        if( grabWhere == GrabBody && ( x0 < sx + SMV_LEFT_DRAG_PIXEL
                                       || x0 >= ex - SMV_RIGHT_DRAG_PIXEL ) )
            return false;   // clip too narrow to have a body clear of the edge bands
        y = laneTop + ( upperHalf ? th/4 : (3*th)/4 );
    }
    int x1 = qContent_->getXPosOfOffset( dropTime );
    if( x0 < 0 || x1 < 0 ) return false;

    // The drag auto-scrolls when the pointer leaves the canvas, which would move
    // the time axis mid-gesture. The window is never shown in test mode, so grow
    // the canvas until both ends are inside it and the drag stays a pure edit.
    int needed = ( x0 > x1 ? x0 : x1 ) + 64;
    if( qContent_->width() < needed )
        qContent_->resize( needed, qContent_->height() > y+th ? qContent_->height() : y+th+64 );

    outGrab = QPoint( x0, y );
    outDrop = QPoint( x1, y );
    return true;
}

bool SStdMixerView::dragClipEdge( int rowIdx, int clipIdx, int grabWhere,
                                  offset_t dropTime, bool upperHalf,
                                  Qt::KeyboardModifiers mods )
{
    QPoint g, d;
    if( !grabPointFor( rowIdx, clipIdx, grabWhere, dropTime, upperHalf, g, d ) )
        return false;
    const int x0 = g.x(), x1 = d.x(), y = g.y();
    auto send = [&]( QEvent::Type type, int x,
                     Qt::MouseButton button, Qt::MouseButtons buttons ) {
        QPointF local( x, y );
        QMouseEvent ev( type, local, qContent_->mapToGlobal( QPointF( x, y ) ),
                        button, buttons, mods );
        QApplication::sendEvent( qContent_, &ev );
    };
    send( QEvent::MouseButtonPress,   x0, Qt::LeftButton, Qt::LeftButton );
    send( QEvent::MouseMove,          x1, Qt::NoButton,   Qt::LeftButton );
    send( QEvent::MouseButtonRelease, x1, Qt::LeftButton, Qt::NoButton );
    return true;
}

// ONE synthesized MOUSE-MOVE at a clip's grab point, so a script can assert
// what the app tells the user is under the pointer (proposal 43 N5 UI). It is
// a HOVER: no button is down, so nothing arms and nothing is edited.
bool SStdMixerView::hoverClipEdge( int rowIdx, int clipIdx, int grabWhere,
                                   Qt::KeyboardModifiers mods )
{
    const STrackRow *row = rowAt( rowIdx );
    if( !row || !row->track || !qContent_ ) return false;
    SLink *clip = NULL;
    int n = 0;
    for( SLink *lk : row->track->childLinks() ) {
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) continue;
        if( n++ == clipIdx ) { clip = lk; break; }
    }
    if( !clip || !clip->hasStartTime() ) return false;
    QPoint g, d;
    if( !grabPointFor( rowIdx, clipIdx, grabWhere, clip->getStartTime(),
                       false, g, d ) )
        return false;
    QMouseEvent ev( QEvent::MouseMove, QPointF( g ),
                    qContent_->mapToGlobal( QPointF( g ) ),
                    Qt::NoButton, Qt::NoButton, mods );
    QApplication::sendEvent( qContent_, &ev );
    return true;
}

bool SStdMixerView::doubleClickClip( int rowIdx, int clipIdx )
{
    const STrackRow *row = rowAt( rowIdx );
    if( !row || !row->track || !qContent_ ) return false;

    // Nested tracks appear in a folder's child list but are lanes, not clips
    // (same skip as dragClipEdge).
    SLink *clip = NULL;
    int n = 0;
    for( SLink *lk : row->track->childLinks() ) {
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) continue;
        if( n++ == clipIdx ) { clip = lk; break; }
    }
    if( !clip || !clip->hasStartTime() || !clip->getSObject().hasDuration() )
        return false;

    // The clip BODY, clear of the edge grab bands — the only spot a plain
    // click (and so a double-click) lands as a select/move gesture rather
    // than a resize.
    offset_t start = clip->getStartTime();
    length_t dur   = clip->getSObject().getDuration();
    int sx = qContent_->getXPosOfOffset( start );
    int ex = qContent_->getXPosOfOffset( start + (offset_t) dur );
    int x  = ( sx + ex ) / 2;
    if( x < 0 ) return false;

    int th = rowHeight( rowIdx );
    int laneTop = qContent_->laneTop( rowIdx );
    int y = laneTop + th / 2;

    // Never shown in test mode; grow the canvas so the point is inside it,
    // exactly as dragClipEdge does.
    int needed = x + 64;
    if( qContent_->width() < needed )
        qContent_->resize( needed, qContent_->height() > y+th ? qContent_->height() : y+th+64 );

    auto send = [&]( QEvent::Type type, Qt::MouseButton button,
                     Qt::MouseButtons buttons ) {
        QPointF local( x, y );
        QMouseEvent ev( type, local, qContent_->mapToGlobal( QPointF( x, y ) ),
                        button, buttons, Qt::NoModifier );
        QApplication::sendEvent( qContent_, &ev );
    };
    // Qt's own double-click delivery is press / release / DBLCLICK / release
    // — the second "press" arrives AS the QEvent::MouseButtonDblClick, never
    // as a second QEvent::MouseButtonPress. That is exactly why a plain
    // single click already selects the clip before mouseDoubleClickEvent
    // ever runs (the selection-follower fix this gesture exercises).
    send( QEvent::MouseButtonPress,    Qt::LeftButton, Qt::LeftButton );
    send( QEvent::MouseButtonRelease,  Qt::LeftButton, Qt::NoButton );
    send( QEvent::MouseButtonDblClick, Qt::LeftButton, Qt::LeftButton );
    send( QEvent::MouseButtonRelease,  Qt::LeftButton, Qt::NoButton );
    return true;
}

bool SStdMixerView::clickLane( STrack *t, offset_t time,
                               Qt::KeyboardModifiers mods )
{
    if( !t || !qContent_ ) return false;
    int rowIdx = rowIndexOfTrack( t );
    if( rowIdx < 0 ) return false;
    int x = qContent_->getXPosOfOffset( time );
    int th = rowHeight( rowIdx );
    int y = qContent_->laneTop( rowIdx ) + th/2;
    if( x < 0 || y < 0 ) return false;

    // The window is never shown in test mode; grow the canvas so the click
    // lands inside it (same reasoning as dragClipEdge / mediaBrowserDrag).
    if( qContent_->width() < x+64 || qContent_->height() < y+th+64 )
        qContent_->resize( qMax( qContent_->width(), x+64 ),
                           qMax( qContent_->height(), y+th+64 ) );

    // Press and release at the SAME point: zero pixel movement, which is what
    // makes mouseReleaseEvent's CLICK_THRESHOLD check treat this as a click
    // rather than a drag.
    auto send = [&]( QEvent::Type type, Qt::MouseButton button,
                     Qt::MouseButtons buttons ) {
        QPointF local( x, y );
        QMouseEvent ev( type, local, qContent_->mapToGlobal( QPointF( x, y ) ),
                        button, buttons, mods );
        QApplication::sendEvent( qContent_, &ev );
    };
    send( QEvent::MouseButtonPress,   Qt::LeftButton, Qt::LeftButton );
    send( QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton );
    return true;
}

// The double-click twin of clickLane() above, and to dragClipEdge()'s
// doubleClickClip() twin: it lands wherever `time` puts it on the track's
// lane, clip or no clip — which is what makes it the ONLY driver that can
// reach the bare-folder-lane double-click (mouseDoubleClickEvent's "no clip
// link at all" branch), the one double-click gesture doubleClickClip() can
// never synthesize because it requires an existing clip to aim at.
bool SStdMixerView::doubleClickLane( STrack *t, offset_t time,
                                     Qt::KeyboardModifiers mods )
{
    if( !t || !qContent_ ) return false;
    int rowIdx = rowIndexOfTrack( t );
    if( rowIdx < 0 ) return false;
    int x = qContent_->getXPosOfOffset( time );
    int th = rowHeight( rowIdx );
    int y = qContent_->laneTop( rowIdx ) + th/2;
    if( x < 0 || y < 0 ) return false;

    // The window is never shown in test mode; grow the canvas so the point
    // lands inside it (same reasoning as clickLane / dragClipEdge).
    if( qContent_->width() < x+64 || qContent_->height() < y+th+64 )
        qContent_->resize( qMax( qContent_->width(), x+64 ),
                           qMax( qContent_->height(), y+th+64 ) );

    auto send = [&]( QEvent::Type type, Qt::MouseButton button,
                     Qt::MouseButtons buttons ) {
        QPointF local( x, y );
        QMouseEvent ev( type, local, qContent_->mapToGlobal( QPointF( x, y ) ),
                        button, buttons, mods );
        QApplication::sendEvent( qContent_, &ev );
    };
    // Same press/release/DBLCLICK/release sequence Qt itself delivers and
    // doubleClickClip() already sends — the leading press selects whatever
    // is under the point (a track, if the lane is bare) before the double-
    // click handler runs.
    send( QEvent::MouseButtonPress,    Qt::LeftButton, Qt::LeftButton );
    send( QEvent::MouseButtonRelease,  Qt::LeftButton, Qt::NoButton );
    send( QEvent::MouseButtonDblClick, Qt::LeftButton, Qt::LeftButton );
    send( QEvent::MouseButtonRelease,  Qt::LeftButton, Qt::NoButton );
    return true;
}

bool SStdMixerView::wheelScroll( int deltaY, Qt::KeyboardModifiers mods )
{
    if( !qContent_ || deltaY == 0 ) return false;
    // Position barely matters here (ScrollHorizontal/ScrollVertical do not
    // read anchorX at all; only ZoomHorizontal's zoom-to-cursor does), but the
    // point must be INSIDE the canvas or QApplication::sendEvent still
    // delivers it — Qt does not hit-test a QWheelEvent against the receiver's
    // rect — so a fixed interior point is fine.
    const QPointF local( 100, 60 );
    QWheelEvent ev( local, qContent_->mapToGlobal( local ), QPoint( 0, 0 ),
                    QPoint( 0, deltaY ), Qt::NoButton, mods,
                    Qt::NoScrollPhase, false );
    QApplication::sendEvent( qContent_, &ev );
    return true;
}

// --- test entry points for the lane geometry ----------------------------

void SStdMixerView::tkSetBaseTrackHeight( int h )
{
    qContent_->setTrackHeight( h );
}

void SStdMixerView::tkSetTopRow( int row )
{
    qContent_->setTopOffset( row );
}

int SStdMixerView::tkVerticalScrollMaximum() const
{
    return qScrollVert_->maximum();
}

QString SStdMixerView::tkCheckLaneAlignment() const
{
    // 1. The row model itself: heights present, prefix sums consistent, and
    //    the y -> row lookup inverting the row -> y one at both lane edges.
    int running = 0;
    for( int i=0; i<rows_.size(); ++i ) {
        const int h = rows_.at( i ).height;
        if( h <= 0 )
            return QString( "row %1: height %2 not set" ).arg( i ).arg( h );
        if( rowTop( i ) != running )
            return QString( "row %1: rowTop %2, expected %3 (sum of heights)" )
                       .arg( i ).arg( rowTop( i ) ).arg( running );
        if( rowAtLaneY( running ) != i )
            return QString( "row %1: rowAtLaneY(top=%2) = %3" )
                       .arg( i ).arg( running ).arg( rowAtLaneY( running ) );
        if( rowAtLaneY( running + h - 1 ) != i )
            return QString( "row %1: rowAtLaneY(bottom=%2) = %3" )
                       .arg( i ).arg( running + h - 1 )
                       .arg( rowAtLaneY( running + h - 1 ) );
        running += h;
    }
    if( totalRowsHeight() != running )
        return QString( "total height %1, expected %2" )
                   .arg( totalRowsHeight() ).arg( running );

    // 2. The heads: one per non-sub-lane row, in row order, each sitting
    //    exactly on the lane the canvas paints for it — through the SAME
    //    ruler-band clamp layoutControlColumn() applies (fix/arranger-ui-
    //    fixes C item 5), so the one row that may legally straddle the
    //    ruler under a pixel-granular scroll is expected CLAMPED here too.
    const int headRow = rulerStraddleHeadRow();
    int c = 0;
    for( int i=0; i<rows_.size(); ++i ) {
        if( rows_.at( i ).isSubLane() ) continue;
        if( c >= controlArray_->size() )
            return QString( "row %1: no head (only %2 heads for %3 rows)" )
                       .arg( i ).arg( controlArray_->size() ).arg( rows_.size() );
        if( controlRow_.value( c, -1 ) != i )
            return QString( "head %1: bound to row %2, expected %3" )
                       .arg( c ).arg( controlRow_.value( c, -1 ) ).arg( i );
        SSMVMixerControl *mc = controlArray_->at( c );
        if( !mc ) return QString( "head %1: null" ).arg( c );
        const QRect g = mc->geometry();
        int wantTop = qContent_->laneTop( i );
        int wantH = laneGroupHeight( i );
        clampHeadToRulerBand( i == headRow, wantTop, wantH );
        const QRect want( 0, wantTop, trackControlWidth_, wantH );
        if( g != want )
            return QString( "head %1 (row %2): geometry %3,%4 %5x%6, "
                            "lane is %7,%8 %9x%10" )
                       .arg( c ).arg( i )
                       .arg( g.x() ).arg( g.y() ).arg( g.width() ).arg( g.height() )
                       .arg( want.x() ).arg( want.y() )
                       .arg( want.width() ).arg( want.height() );
        ++c;
    }
    const QString autoProblem = checkAutomationRows();
    if( !autoProblem.isEmpty() ) return autoProblem;

    if( c != controlArray_->size() )
        return QString( "%1 heads for %2 track lanes" )
                   .arg( controlArray_->size() ).arg( c );
    return QString();
}

// `ensureSCut()` LIVED HERE and is deleted (proposal 42 M1). It tested the
// class NAME against the literal "SCut" and wrapped anything else in a new
// `SCut`, deleting the old link — during a mouse-move, from seven gesture call
// sites. It was the principal factory of the wrapped take-column shape, it
// turned a MIDI clip into an audio clip pinned to TIME (measured: `startTicks`
// gone after one border drag), it appended the rewritten link so every clip
// path recorded in the undo stack then addressed a different clip, and it left
// the old object's `durationChanged` connected to the lane. Its callers now
// resolve an editable target instead (`clipEditTargetOf`), which needs no tree
// rewrite because a window and a take column are both already editable.
//
// Raw content on a lane — its original purpose — is produced by no verb; such
// a placement now resolves to no window and its gestures are no-ops.

// The scrollable horizontal TIMELINE EXTENT, in project frames (fix/
// arranger-ui-fixes B1/B3). See the header comment on the declaration.
offset_t SStdMixerView::horizontalExtent() const
{
    const offset_t dur = ( model_ && model_->hasDuration() ) ? model_->getDuration() : (offset_t) 0;
    const int srate = model_ ? model_->getProject().getSRate() : 48000;
    offset_t visSpan = 0;
    if( qContent_ && qContent_->getSecondWidth() > 0.0 ) {
        visSpan = (offset_t)( (double) qContent_->width() / qContent_->getSecondWidth() * srate );
    }
    const offset_t cur = qContent_ ? qContent_->getLeftOffset() : (offset_t) 0;
    offset_t extent = qMax( dur, cur + visSpan );
    // Floor (B3): the old bug's own trigger was `dur <= 1`, which made a
    // division by `dur` blow up to a near-infinite `double` and then an
    // out-of-range double->int conversion (undefined behaviour) that landed
    // `maximum` on 0 — the bar dead while the wheel still panned. Nothing
    // here divides by `dur` any more, but an empty/near-empty arrangement
    // should still offer a SANE domain rather than a near-[0,1] one: ten
    // seconds at the project's own rate.
    extent = qMax( extent, (offset_t) srate * 10 );
    return extent;
}

void SStdMixerView::recalcPageStep()
{
    // qContent_ was resized. Recalc scrollbars.
    int w = qContent_->width();
    int srate = model_ ? model_->getProject().getSRate() : 48000;

    // Horizontal (fix/arranger-ui-fixes B): the bar's domain is FRAMES, so
    // the page step is "how many project frames fit in the viewport at the
    // current zoom" directly — no HSliderRange rescale — and the maximum is
    // the scrollable EXTENT (B1) minus that.
    double dw = (double) w * srate / qContent_->getSecondWidth();
    offset_t pageFrames = (offset_t) dw;
    if( pageFrames < 0 ) pageFrames = 0;
    if( pageFrames > 0x7fffffff ) pageFrames = 0x7fffffff;
    const int ps = (int) pageFrames;
    qScrollHoriz_->setPageStep( ps );
    qScrollHoriz_->setSingleStep( (ps/10)+1 );
    const offset_t extent = horizontalExtent();
    offset_t maxFrames = ( extent > (offset_t) ps ) ? extent - (offset_t) ps : (offset_t) 0;
    if( maxFrames > 0x7fffffff ) maxFrames = 0x7fffffff;
    qScrollHoriz_->setMaximum( (int) maxFrames );

    // Vertical (fix/arranger-ui-fixes C): pixel-granular, so the page step is
    // simply the visible pixel height below the ruler.
    const int availPx = qMax( 0, qContent_->height() - SMV_TIME_RULER_HEIGHT );
    qScrollVert_->setPageStep( availPx );
    qScrollVert_->setSingleStep( qMax( 1, getTrackHeight() / 8 ) );
    qScrollVert_->setMaximum( verticalScrollMaximum( availPx ) );
}

// See the header for why this exists: a single definition of "how far may
// qScrollVert_ go" shared by recalcPageStep() and nTracksChanged(), so a
// track add/remove cannot silently compute a different answer. Pixel-granular
// now (fix/arranger-ui-fixes C) — the old "+1 row of headroom" hack existed
// only because ROW granularity could not reach a partially visible last row;
// a pixel offset reaches the true content bottom directly.
int SStdMixerView::verticalScrollMaximum( int availPx ) const
{
    return qMax( 0, totalRowsHeight() - availPx );
}

void SStdMixerView::viewResized()
{
#if 0
    // qContent_ was resized. Recalc scrollbars.
    int w = qContent_->width();
    int h = qContent_->height();
    // Calc new pageStep.
    int srate = model_ ? model_->getProject().getSRate() : 48000;
    double dw = w;
    dw = dw * srate / qContent_->getSecondWidth();
    offset_t lw = (offset_t) (dw);
    if( lw>0x7fffffff ) lw = 0x7fffffff;
    qScrollHoriz_->setPageStep( lw );
    qScrollHoriz_->setMaxValue( model_->getDuration()-lw );
    h /= qContent_->getTrackHeight();
    qScrollVert_->setPageStep( h );
    qScrollVert_->setMaxValue( model_->getNTracks()-h );
#else
    recalcPageStep();
#endif
}

void SStdMixerView::zoomInHor()
{
    // FIXME: Range checking.
    double secWidth = qContent_->getSecondWidth();
    // FIXME: Configure this
    secWidth *= 1.5;
    qContent_->setSecondWidth( secWidth );
}

void SStdMixerView::zoomOutHor()
{
    // FIXME: Range checking.
    double secWidth = qContent_->getSecondWidth();
    // FIXME: Configure this
    secWidth /= 1.5;
    qContent_->setSecondWidth( secWidth );
}

// Vertical zoom scales the BASE height; per-track scales ride on top of it, so
// relative lane sizes survive zooming. setTrackHeight() rebuilds the row
// geometry and replaces the heads — nothing to do here beyond asking.
void SStdMixerView::zoomInVert()
{
    // FIXME: Range checking.
    int h = qContent_->getTrackHeight();
    // FIXME: Configure this
    qContent_->setTrackHeight( (h*3)/2 );
}

void SStdMixerView::zoomOutVert()
{
    // FIXME: Range checking.
    int h = qContent_->getTrackHeight();
    // FIXME: Configure this
    qContent_->setTrackHeight( (h*2)/3 );
}

void SStdMixerView::onProjectTempoChanged( double bpmTempo )
{
    // The map moved with the tempo; the snap grid reads it (proposal 37 P4).
    if( currentSnapSpec_ && model_ )
        currentSnapSpec_->setTempoMap( model_->getProject().tempoMap() );

    STimeGridSpec tgs = getTimeGridSpec();
    double oldTempo = tgs.getBPM();
    if ( bpmTempo != oldTempo ) {
        tgs.setBPM( bpmTempo );
        setTimeGridSpec( tgs );
        // FIXME: This should be superfluous, as the timeGridSpec_
        // changed signal should do it.
        update();
    }
}

/**
 * Align the offset passed according to the current grid settings.
 */
offset_t SStdMixerView::alignTime( offset_t o )
{
    // Snap is a per-project property (toolbar palette / snap-to-grid action).
    if( model_ && !model_->getProject().prop( SProjectProps::SnapToGrid, true ).toBool() ) {
        return o;
    }
    if( currentSnapSpec_ ) {
        return currentSnapSpec_->alignTime( o );
    } else {
        qWarning( "SStdMixerView::alignTime(): No snap spec "
                  "set for mixer view.\n" );
        return o;
    }
}

int SStdMixerView::getTrackHeight() const 
{
    return qContent_->getTrackHeight();
}

idx_t SSnapSpec::getBeatSubDiv() const
{
    return beatSubDiv_;
}

int SSnapSpec::getSnapMethod() const
{
    return snapMethod_;
}

void SSnapSpec::setBeatSubDiv( idx_t subDiv )
{
    beatSubDiv_ = subDiv;
    emit beatSubDivChanged( subDiv );
}

void SSnapSpec::setSnapMethod( int snapMethod )
{
    snapMethod_ = snapMethod;
    emit snapMethodChanged( snapMethod );
}

void SSnapSpec::setGridDivision( const QString &division )
{
    gridDivision_ = division;
}

void SSnapSpec::setTempoMap( const twTempoMap &map )
{
    tempoMap_ = map;
    haveTempoMap_ = true;
}

// The snap step in frames for the current division, or 0 when there is none.
// Exact rational through the tempo map (D2), floored once - the same shape
// SMidiCut converts a window with.
offset_t SSnapSpec::divisionFrames_() const
{
    if( gridDivision_.isEmpty() || !haveTempoMap_ ) return 0;
    const qint64 ticks =
        SQuantizeNotesAction::gridTicks( gridDivision_, tempoMap_.ppq() );
    if( ticks <= 0 ) return 0;
    const int64_t f =
        tempoMap_.ticksToFrames( TickLen( (int64_t) ticks ), sampleRate_ )
                 .floorToInt();
    return f > 0 ? (offset_t) f : (offset_t) 0;
}

offset_t SSnapSpec::alignTime( offset_t o )
{
    // A named DIVISION wins when there is one (proposal 37 P4). With none, this
    // is byte-for-byte the pre-36 beat snap, which is what keeps every
    // committed case's snapped positions unchanged.
    if( ( snapMethod_ & SnapToBeats ) ) {
        const offset_t wo = divisionFrames_();
        if( wo > 0 ) {
            offset_t onew = ( ( o + ( wo >> 1 ) ) / wo ) * wo;
            length_t diff = onew - o;
            if( diff < 0 ) diff = -diff;
            if( ( (offset_t) diff ) < wo / 2 ) o = onew;
            return o;
        }
    }
    if( snapMethod_ & SnapToBeats ) {
        //int beatsPerBar = tgs_.getEmphasizeGrids( 0 );
        //if( beatsPerBar<=0 ) beatsPerBar = 1;
        double w = tgs_.getTimeGridWidth();
        //w *= beatsPerBar;
        w *= sampleRate_;
        offset_t wo = (offset_t) (w);
        if( wo<=0 ) wo = 1;
        offset_t onew;
        onew = o + (wo>>1);
        onew /= wo;
        onew *= wo;        
        length_t diff = onew-o;
        if( diff<0 ) diff=-diff;
        if( ((offset_t)diff)<wo/2 ) o = onew;
    }
    return o;
}


void STimeGridSpec::setBPM( double bpm )
{
    if( bpm<1. ) bpm = 1.;
    bpm = 60./bpm;
    setTimeGridWidth( bpm );
}

double STimeGridSpec::getBPM() const
{
    double bpm = getTimeGridWidth();
    if( bpm<0.0000001 ) bpm = 0.0000001;
    bpm = 60./bpm;
    return bpm;
}

SSnapSpec::~SSnapSpec()
{
}

SSnapSpec::SSnapSpec( STimeGridSpec &tgs )
    : QObject(),
      beatSubDiv_( 1 ),
      snapMethod_( SnapToBeats ),
      sampleRate_( 48000 ),
      tgs_( tgs )
{
}

SMVActualView::InlineRenderContext::~InlineRenderContext()
{
}

SMVActualView::InlineRenderContext::InlineRenderContext( SMVActualView &smv, QPainter &painter )
    : SRenderContext( painter ),
      mixerView_( smv )
{    
}

SMVActualView::~SMVActualView()
{
}

SMVActualView::SMVActualView( QWidget *parent, SStdMixerView &smv )
    : QWidget( parent ),
      smv_( smv )
{
    // The lane area must also stay usable regardless of layout ordering, so it cannot
    // collapse within the grid (see the matching note in SStdMixerView's constructor).
    setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );
    setMinimumSize( 150, 60 );

    // setBackgroundMode( NoBackground );
    qGlobalPopup_ = new QMenu( this );
    QObject::connect( qGlobalPopup_, SIGNAL( aboutToShow() ),
                      this, SLOT( ctGlobalShow() ) );

    // Context menu for the time-range bar (top ruler).
    qRangePopup_ = new QMenu( this );
    qRangePopup_->addAction( "Set &BPM...", this, SLOT( ctRangeSetBPM() ) );
    // Grid division (proposal 37 P4) — the arranger's snap step, named the way
    // `quantize-notes grid=` and the event editor's grid name one. "Beat" is
    // the empty division, i.e. the pre-36 behaviour.
    {
        QMenu *divMenu = qRangePopup_->addMenu( "&Grid division" );
        static const char *const kDivs[] = {
            "", "1/1", "1/2", "1/4", "1/4t", "1/8", "1/8t", "1/16", "1/16t",
            "1/32", "1/32t"
        };
        for( const char *d : kDivs ) {
            const QString div = QLatin1String( d );
            QAction *a = divMenu->addAction( div.isEmpty() ? QStringLiteral( "Beat" )
                                                           : div );
            a->setCheckable( true );
            QObject::connect( a, &QAction::triggered, this, [this, div] {
                if( smv_.snapSpec() ) smv_.snapSpec()->setGridDivision( div );
                update();
            } );
        }
        QObject::connect( divMenu, &QMenu::aboutToShow, this, [this, divMenu] {
            const QString cur = smv_.snapSpec() ? smv_.snapSpec()->gridDivision()
                                                : QString();
            for( QAction *a : divMenu->actions() ) {
                const QString name = a->text();
                a->setChecked( cur.isEmpty() ? name == QStringLiteral( "Beat" )
                                             : name == cur );
            }
        } );
    }
    qRangePopup_->addSeparator();
    qRangeActClear_ = qRangePopup_->addAction( "&Clear range", this, SLOT( ctRangeClear() ) );
    qRangePopup_->addSeparator();
    QAction *actMode = qRangePopup_->addAction( "Time display: &Bars" );
    actMode->setCheckable(true);
    QObject::connect( qRangePopup_, &QMenu::aboutToShow, this,
                      [this, actMode]() {
                          if (smv_.getModel()) {
                              bool isBars = smv_.getModel()->getProject().prop(
                                  SProjectProps::RulerMode, "bars").toString() == "bars";
                              actMode->setChecked(isBars);
                          }
                      } );
    QObject::connect( actMode, &QAction::toggled, this,
                      [this](bool checked) {
                          if (smv_.getModel())
                              smv_.getModel()->getProject().setProp(
                                  SProjectProps::RulerMode, checked ? "bars" : "time");
                      } );

    rangeValid_ = false;
    rangeStart_ = rangeEnd_ = 0;
    rangeDrag_ = RangeNone;
    // Restore a saved range marker for this project (the view is rebuilt per
    // project, and the project is fully loaded before this widget is created).
    loadRangeFromProject();

    // Track the mouse with no button held so the cursor can telegraph the
    // clip-edit gesture under the pointer (resize / slip / stretch / loop).
    setMouseTracking( true );

    // Accept drag-drop from the resource list (assets and external files).
    setAcceptDrops(true);

    trackHeight_ = SMV_DEFAULT_TRACK_HEIGHT;   // per-track scales ride on it
    upperLeftY_ = 0;
    topRow_ = 0;
    // Zoom / horizontal pan (fix/track-list-polish m): secondWidth_,
    // upperLeftOffset_ and upperLeftX_ used to be hard-coded here (30., 0, 0).
    // loadViewStateFromProject() sets them from what a previous save left in
    // the project's property dict, falling back to those same numbers when
    // there is nothing saved (a fresh project, or one written before this).
    loadViewStateFromProject();

    QObject::connect( smv_.model_, SIGNAL( trackInserted( int, STrack & ) ),
                      SLOT( update() ) );
    QObject::connect( smv_.model_, SIGNAL( trackRemoved( int, STrack & ) ),
                      SLOT( update() ) );

    // Repaint when a project property (e.g. grid visibility) changes. Extra
    // signal args are dropped by the connection; repainting is cheap.
    QObject::connect( &smv_.model_->getProject(),
                      SIGNAL( propertyChanged( QString, QVariant ) ),
                      this, SLOT( update() ) );

    // Repaint on any arrangement change (an applied action, or a mute/solo
    // toggle). Cached renders (asset captures) have already been invalidated by
    // the same signal, so the repaint re-pulls a fresh waveform preview.
    QObject::connect( &smv_.model_->getProject(),
                      SIGNAL( arrangementChanged() ),
                      this, SLOT( update() ) );

    // Repaint when an async capture revalidation lands (worker thread, queued
    // delivery): previews computed in the background become visible without
    // waiting for an unrelated repaint (e.g. the playhead moving).
    QObject::connect( &smv_.model_->getProject(),
                      SIGNAL( captureRevalidated() ),
                      this, SLOT( update() ) );

    // Persist zoom / horizontal pan across save/load (fix/track-list-polish
    // m). Every path that changes them — the wheel, the zoom buttons, the
    // scrollbar — funnels through setSecondWidth()/setLeftOffset(), so
    // hooking their signals here covers all of them without hunting down
    // each call site, the same way saveRangeToProject() is called from the
    // one place a range drag ends.
    QObject::connect( this, &SMVActualView::secondWidthChanged,
                      this, [this]( double ) { saveViewStateToProject(); } );
    QObject::connect( this, &SMVActualView::leftOffsetChanged,
                      this, [this]( offset_t ) { saveViewStateToProject(); } );

    // Mouse-wheel navigation config: cache now and refresh whenever the user
    // changes it in the options dialog.
    loadWheelConfig();
    QObject::connect( &SSettings::instance(), &SSettings::changed,
                      this, [this]( const QString & ){ loadWheelConfig(); } );
}

void SMVActualView::loadWheelConfig()
{
    SSettings &s = SSettings::instance();
    wheelPlain_        = s.value( SOpt::WheelPlain,     SOpt::def( SOpt::WheelPlain ) ).toInt();
    wheelShift_        = s.value( SOpt::WheelShift,     SOpt::def( SOpt::WheelShift ) ).toInt();
    wheelCtrl_         = s.value( SOpt::WheelCtrl,      SOpt::def( SOpt::WheelCtrl ) ).toInt();
    wheelCtrlShift_    = s.value( SOpt::WheelCtrlShift, SOpt::def( SOpt::WheelCtrlShift ) ).toInt();
    wheelZoomToCursor_ = s.value( SOpt::ZoomToCursor,  SOpt::def( SOpt::ZoomToCursor ) ).toBool();
    wheelInvertZoom_   = s.value( SOpt::InvertZoom,    SOpt::def( SOpt::InvertZoom ) ).toBool();
    followPlayhead_    = s.value( SOpt::FollowPlayhead, SOpt::def( SOpt::FollowPlayhead ) ).toBool();

    // One sensitivity, four gestures. Clamped to the spin box's own range, which
    // also catches a hand-edited INI: a zero or negative factor would divide the
    // scroll threshold by zero and stall every gesture.
    int pct = s.value( SOpt::WheelSensitivityPct,
                       SOpt::def( SOpt::WheelSensitivityPct ) ).toInt();
    pct = qBound( 10, pct, 500 );
    wheelSensitivity_ = pct / 100.0;

    // Scroll and pan are LINEAR in the gesture, so sensitivity multiplies the
    // step directly (vertical scroll joined this since fix/arranger-ui-fixes
    // C — it used to divide a lane-count threshold, kept here in the same
    // spot for the reader tracing the four gestures together); zoom is
    // multiplicative, so it raises the per-notch factor to the power (see the
    // constants above).
    //
    // The pow() calls are guarded by an exact `wheelSensitivity_ == 1.0` check
    // rather than trusted, because pow(x, 1.0) returning exactly x is a
    // quality-of-implementation property, not a guarantee — and "the default
    // feel does not move" is the whole argument for shipping this option.
    wheelZoomHFactor_ = ( wheelSensitivity_ == 1.0 )
                        ? SMV_WHEEL_ZOOM_H_BASE
                        : std::pow( SMV_WHEEL_ZOOM_H_BASE, wheelSensitivity_ );
    wheelZoomVFactor_ = ( wheelSensitivity_ == 1.0 )
                        ? SMV_WHEEL_ZOOM_V_BASE
                        : std::pow( SMV_WHEEL_ZOOM_V_BASE, wheelSensitivity_ );
}

int SMVActualView::wheelActionFor( Qt::KeyboardModifiers mods ) const
{
    // The "ctrl" wheel slot maps to the platform primary modifier: Command
    // (Qt::MetaModifier) on macOS, Ctrl (Qt::ControlModifier) elsewhere. On macOS
    // plain Ctrl+wheel is deliberately NOT treated as primary here — it is left to
    // the OS accessibility screen-zoom (see the early-out in wheelEvent()).
    bool ctrl  = hasPrimaryMod( mods );
    bool shift = mods & Qt::ShiftModifier;
    if( ctrl && shift ) return wheelCtrlShift_;
    if( ctrl )          return wheelCtrl_;
    if( shift )         return wheelShift_;
    return wheelPlain_;
}

QString SMVActualView::describeWheelActions() const
{
    auto actionName = [](int action) -> QString {
        switch( action ) {
        case SOpt::ScrollVertical:   return "Scroll V";
        case SOpt::ScrollHorizontal: return "Pan";
        case SOpt::ZoomHorizontal:   return "Zoom H";
        case SOpt::ZoomVertical:     return "Zoom V";
        default:                      return QString();
        }
    };

    QStringList parts;
    QString plain = actionName( wheelPlain_ );
    if( !plain.isEmpty() ) parts << plain;

    QString shift = actionName( wheelShift_ );
    if( !shift.isEmpty() ) parts << ("Shift = " + shift);

    QString ctrl = actionName( wheelCtrl_ );
    if( !ctrl.isEmpty() ) parts << ("Ctrl = " + ctrl);

    QString ctrlShift = actionName( wheelCtrlShift_ );
    if( !ctrlShift.isEmpty() ) parts << ("Ctrl+Shift = " + ctrlShift);

    return "Wheel: " + parts.join( ", " );
}

void SMVActualView::wheelEvent( QWheelEvent *ev )
{
    // The pointer really is over the canvas here, so a time under it exists.
    if( !applyWheel( ev, (int) ev->position().x() ) )
        QWidget::wheelEvent( ev );
}

bool SMVActualView::applyWheel( QWheelEvent *ev, int anchorX )
{
    int dy = ev->angleDelta().y();
    int dx = ev->angleDelta().x();

    // On macOS with physical mouse wheel, certain modifier combinations swap X/Y reporting.
    // When Y delta is zero but X delta is non-zero, treat X as Y (for vertical operations).
    // This handles Shift+wheel, Cmd+Shift+wheel, etc. on physical mouse wheels.
    if( dy == 0 && dx != 0 ) {
        dy = dx;
    }

#ifdef Q_OS_MACOS
    // The physical Control key is Qt::MetaModifier on macOS, and physical-Ctrl+scroll
    // is the system "zoom using scroll gesture" accessibility feature. Don't consume
    // it — leave that scroll to the OS. The app's own zoom is on ⌘+wheel, which is
    // Qt::ControlModifier here and falls through.
    if( ev->modifiers() & Qt::MetaModifier ) {
        return false;
    }
#endif

    if( dy == 0 ) return false;
    int dir = (dy > 0) ? +1 : -1;   // +1 = wheel away from the user ("up")

    int action = wheelActionFor( ev->modifiers() );

    switch( action ) {

    case SOpt::ScrollVertical: {
        // Pixel-granular (fix/arranger-ui-fixes C): a notch moves a fraction
        // of the CURRENT base lane height, scaled by the wheel sensitivity —
        // mirrors ScrollHorizontal right below it. No accumulator is needed
        // (unlike the old lane-quantised step): any sub-notch delta from a
        // trackpad already maps to a valid pixel amount, so the thumb (and
        // the canvas) can land on a partially visible top row, same as a
        // drag. +delta = wheel up = scroll toward upper rows.
        if( smv_.qScrollVert_ ) {
            const double pxPerNotch = (double) trackHeight_ * SMV_WHEEL_VSCROLL_FRACTION * wheelSensitivity_;
            const double px = (double) dy / 120.0 * pxPerNotch;
            const int deltaPx = (int)( px + ( px >= 0.0 ? 0.5 : -0.5 ) );
            if( deltaPx != 0 )
                smv_.qScrollVert_->setValue( smv_.qScrollVert_->value() - deltaPx );
        }
        break;
    }

    case SOpt::ScrollHorizontal: {
        // Pan the timeline by ~1/8 of the visible span per notch, scaled by the
        // wheel sensitivity. The integer division stays FIRST so that at 100 %
        // the scaling is a multiplication by exactly 1.0 of the very same value.
        offset_t span = getTimeOf( width() ) - getTimeOf( 0 );
        offset_t step = (offset_t)( (double)( span / 8 ) * wheelSensitivity_ );
        if( step < 1 ) step = 1;
        offset_t cur = upperLeftOffset_;
        offset_t next = (dir > 0) ? ( cur > step ? cur - step : 0 )   // up = earlier
                                  : cur + step;
        armFollowHold();   // AC-g1: a hand-driven pan, not an auto-follow re-page
        setLeftOffset( next );
        break;
    }

    case SOpt::ZoomHorizontal: {
        bool in = (dir > 0);
        if( wheelInvertZoom_ ) in = !in;
        double newW = secondWidth_ * ( in ? wheelZoomHFactor_ : 1.0 / wheelZoomHFactor_ );
        // anchorX < 0 = the pointer is not over the canvas (the gesture came
        // from the track-head column), so there is no time under it to hold
        // still: fall back to keeping the left edge, as zoom-to-cursor-off does.
        if( wheelZoomToCursor_ && anchorX >= 0 ) {
            int mouseX = anchorX;
            offset_t t = getTimeOf( mouseX );          // time under cursor (pre-zoom)
            setSecondWidth( newW );
            int srate = smv_.model_ ? smv_.model_->getProject().getSRate() : 48000;
            double ahead = ((double) mouseX) / newW * srate;
            offset_t left = ( (double) t > ahead ) ? (offset_t)( (double) t - ahead ) : 0;
            setLeftOffset( left );
        } else {
            setSecondWidth( newW );
        }
        break;
    }

    case SOpt::ZoomVertical: {
        bool in = (dir > 0);
        if( wheelInvertZoom_ ) in = !in;
        // Divide rather than multiply by the reciprocal on the way out: at the
        // default 1.5 that reproduces the old (h*3)/2 and (h*2)/3 exactly, where
        // h * (1.0/1.5) truncates one below on every h divisible by 3.
        int h = in ? (int)( trackHeight_ * wheelZoomVFactor_ )
                   : (int)( trackHeight_ / wheelZoomVFactor_ );
        // Track height is an integer, so a gentle sensitivity can round back to
        // where it started and the zoom simply stops responding. Move a pixel
        // instead. Never reached at 100 % — the floor of 6 px is already past
        // the point where 1.5x rounds to nothing.
        if( in  && h <= trackHeight_ ) h = trackHeight_ + 1;
        if( !in && h >= trackHeight_ ) h = trackHeight_ - 1;
        if( h < 6 ) h = 6;
        setTrackHeight( h );   // rebuilds the row geometry + the head column
        break;
    }

    case SOpt::None:
    default:
        return false;
    }
    ev->accept();

    // Post a hint showing available wheel actions and modifiers
    SMainWindow *mw = dynamic_cast<SMainWindow*>(
        QApplication::activeWindow() );
    if( mw ) {
        mw->postHint( describeWheelActions() );
    }
    return true;
}

void SMVActualView::dragEnterEvent(QDragEnterEvent *e)
{
    // Our own resource drags, plus OS file drops (proposal 37 6.1: a .mid
    // dragged in from a file manager is the natural way to get one in).
    if (e->mimeData()->hasFormat(QStringLiteral("application/x-smaragd-resource"))
        || e->mimeData()->hasUrls()) {
        e->acceptProposedAction();
    }
}

void SMVActualView::dragMoveEvent(QDragMoveEvent *e)
{
    if (e->mimeData()->hasFormat(QStringLiteral("application/x-smaragd-resource"))) {
        e->acceptProposedAction();
    }
}

void SMVActualView::dropEvent(QDropEvent *e)
{
    const QMimeData *mimeData = e->mimeData();
    QString payload;
    if (mimeData->hasFormat(QStringLiteral("application/x-smaragd-resource"))) {
        payload = QString::fromUtf8(
            mimeData->data(QStringLiteral("application/x-smaragd-resource")));
    } else if (mimeData->hasUrls()) {
        // An OS file drop (proposal 37 6.1). Normalised into the same "file:"
        // payload the internal drag uses, so there is one placement path and
        // one extension dispatch below it - a .mid becomes an event clip
        // because SProject::linkToFile says so, not because this branch knows.
        for (const QUrl &url : mimeData->urls()) {
            if (!url.isLocalFile()) continue;
            payload = QStringLiteral("file:") + url.toLocalFile();
            break;   // one clip per drop; multi-file drops are a later gesture
        }
    }
    if (payload.isEmpty()) {
        return;
    }

    e->acceptProposedAction();

    // Compute drop position (time + track).
    offset_t timePos = smv_.alignTime(getTimeOf((int)e->position().x()));
    int rowIdx = rowAtViewY( (int) e->position().y() );

    // Get the target track from the row. A drop on EMPTY canvas space below
    // the last lane (anywhere !smv_.rowAt(...) resolves, e.g. under the last
    // track) creates a new track first and places the item on it there — the
    // same gesture mouseDoubleClickEvent() drives via ctAddTrackBelowLast(),
    // rather than silently doing nothing as this used to. A drop still over
    // the time ruler is refused: that is not lane space at all.
    const STrackRow *row = smv_.rowAt(rowIdx);
    STrack *track = row ? row->track : nullptr;
    if (!track) {
        if ((int) e->position().y() < SMV_TIME_RULER_HEIGHT) {
            return;
        }
        track = smv_.ctAddTrackBelowLast();
        if (!track) {
            return;
        }
    }

    SProject *project = SApplication::app().getCurrentProject();
    if (!project) {
        return;
    }

    // Resolve the track's path from the root mixer.
    SObject *root = splacements::rootContainer(project);
    if (!root || !root->isPathContainer()) {
        return;
    }

    using namespace strackpath;
    QList<int> trackPath = pathOf(root, track);
    // pathOf() returns {} for "the root itself" as well as "not found", and a
    // drop row is always a track — so empty means not found. Refuse rather than
    // let it resolve to the root mixer and drop the clip on the master.
    if (trackPath.isEmpty()) {
        return;
    }

    // Parse the MIME payload and submit the appropriate action.
    if (payload.startsWith(QStringLiteral("asset:"))) {
        QString assetName = payload.mid(6);
        // Friendly cycle guard: placing an asset inside its own source container
        // (or a descendant) would self-reference. Refuse with a hint rather than
        // letting SPlaceAssetAction::apply() silently no-op. (apply() is the
        // authoritative backstop for scripted placements.)
        SObject *assetBody = SApplication::app().getCurrentProject()
                                 ? SApplication::app().getCurrentProject()->asset(assetName)
                                 : nullptr;
        bool cycle = false;
        if (SCut *assetCut = dynamic_cast<SCut*>(assetBody)) {
            SObject *container = &assetCut->getContent();
            cycle = (container == root);
            if (!cycle) {
                if (STrack *ct = dynamic_cast<STrack*>(container))
                    cycle = isSelfOrDescendant(track, ct);
            }
        }
        if (cycle) {
            if (QMainWindow *mw = qobject_cast<QMainWindow*>(window()))
                mw->statusBar()->showMessage(
                    "Can't place an asset inside its own track.", 4000);
            update();
            return;
        }
        stimeline::submitActive(new SPlaceAssetAction(assetName, trackPath, timePos));
    } else if (payload.startsWith(QStringLiteral("file:"))) {
        QString filePath = payload.mid(5);
        // For file drops, use SAddSampleAction (same as Insert Sample dialog).
        // The same trackPath the asset branch above uses: an index-path, so a
        // drop onto a track nested in a folder lands too. This was
        // mixer->indexOfChildObject(), which only sees top-level children and
        // made a drop onto a grouped lane a silent no-op.
        stimeline::submitActive(new SAddSampleAction(trackPath, filePath, timePos));
    } else if (payload.startsWith(QStringLiteral("media:"))) {
        // A REMOTE row out of the media browser (proposal 38 §B.5). Everything
        // under this line is app/media's: get a local path, then submit the
        // SAME SAddSampleAction the file: branch above does. No new action
        // type, no placeholder clip, no SCut work. It is handed the TRACK, not
        // trackPath -- a pending placement must hold the target's identity, not
        // a position in a tree the user can edit during a 40 MB download (T18).
        smediadrop::placeWhenLocal(SMediaRef::fromUri(payload.mid(6)),
                                   track, timePos);
    }

    // Repaint the lanes so the newly placed clip becomes visible — the view's
    // update() is wired to track insert/remove, not clip additions (mirrors the
    // explicit qContent_->update() in ctInsertSample).
    update();
}


SStdMixerView::SStdMixerView( QWidget *parent, SStdMixer *model )
    : QWidget( parent ),
      model_( model ),
      snapToTimeGrid_( true ),
      currentSnapSpec_( NULL )
{
    // The mixer view is the window's primary content and must never be collapsible
    // to a sliver by the dock layout. Without an explicit minimum + expanding policy,
    // QMainWindow can — depending on the order of restoreState()/setCentralWidget()/
    // show() — squeeze this central widget so the lane area ends up ~47x9 (tracks are
    // drawn but into a few pixels, i.e. invisible), and because the extern-file dock is
    // also expanding it never recovers on resize. An expanding policy with a usable
    // minimum keeps the layout correct under any ordering.
    setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );
    setMinimumSize( 300, 150 );

    controlArray_ = new QVector<SSMVMixerControl*>();

    qGridLayout_ = new QGridLayout( this /* , 4, 5 */ );    
    qContent_ = new SMVActualView( this, *this );
    // Clicking the arranger focuses it, which is what scopes the +/- pitch
    // shortcuts below to this widget: a bare "+"/"-" as a WINDOW shortcut would
    // be stolen from the tempo spin box and any other numeric field the moment
    // the user typed one there.
    qContent_->setFocusPolicy( Qt::ClickFocus );
    qScrollVert_ = new QScrollBar(
        /* 0, 0, 0, 1, 1, */
        Qt::Vertical, this );
    qScrollHoriz_ = new QScrollBar(
        Qt::Horizontal, this );

    QSize hSliderSize = qScrollHoriz_->sizeHint();
    QSize vSliderSize = qScrollVert_->sizeHint();

    QSize scrollButtonSize( hSliderSize.height(), vSliderSize.width() );

    qHZoomIn_ = new QToolButton( this ); 
    qHZoomIn_->setIcon( QIcon( QPixmap((const char **)zoomin_xpm) ) );
    qHZoomIn_->setFixedSize( scrollButtonSize );
    QObject::connect( qHZoomIn_, SIGNAL( clicked() ), this, SLOT( zoomInHor() ) );
    qHZoomOut_ = new QToolButton( this );
    qHZoomOut_->setIcon( QIcon( QPixmap((const char **)zoomout_xpm) ) );
    qHZoomOut_->setFixedSize( scrollButtonSize );
    QObject::connect( qHZoomOut_, SIGNAL( clicked() ), this, SLOT( zoomOutHor() ) );
    qVZoomIn_ = new QToolButton( this );
    qVZoomIn_->setIcon( QIcon( QPixmap((const char **)zoomin_xpm) ) );
    qVZoomIn_->setFixedSize( scrollButtonSize );
    QObject::connect( qVZoomIn_, SIGNAL( clicked() ), this, SLOT( zoomInVert() ) );
    qVZoomOut_ = new QToolButton( this ); 
    qVZoomOut_->setIcon( QIcon( QPixmap((const char **)zoomout_xpm) ) );
    qVZoomOut_->setFixedSize( scrollButtonSize );
    QObject::connect( qVZoomOut_, SIGNAL( clicked() ), this, SLOT( zoomOutVert() ) );

    qZoomTotal_ = new QToolButton( this ); 
    qZoomTotal_->setFixedSize( scrollButtonSize );
    QObject::connect( qScrollHoriz_, SIGNAL( valueChanged( int ) ),
                      this, SLOT( timeSliderMoved( int ) ) );
    // AC-g1: actionTriggered, not valueChanged — see the header comment on
    // onHScrollUserAction for why the distinction is load-bearing.
    QObject::connect( qScrollHoriz_, SIGNAL( actionTriggered( int ) ),
                      this, SLOT( onHScrollUserAction( int ) ) );
    QObject::connect( qScrollVert_, SIGNAL( valueChanged( int ) ),
                      this, SLOT( trackSliderMoved( int ) ) );

    qTrackControlBoxHolder_ = new QWidget( this );
    // Allow track control column to expand and fill entire left width
    qTrackControlBoxHolder_->setMinimumWidth( TRACK_CTRL_WIDTH_MINIMAL );
    // No maximum width - column fills all available space
    qTrackControlBoxHolder_->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Expanding );
    // Double-clicking the blank area below the track heads adds a new track.
    qTrackControlBoxHolder_->installEventFilter( this );

    // Vertical layout for the track control holder. It carries the track
    // controls alone; the track detail panel used to sit below them and now
    // lives in its own dock on the main window.
    QVBoxLayout *trackHolderLayout = new QVBoxLayout( qTrackControlBoxHolder_ );
    trackHolderLayout->setContentsMargins( 0, 0, 0, 0 );
    trackHolderLayout->setSpacing( 0 );

    // Add to grid layout spanning all rows
    qGridLayout_->addWidget(
        qTrackControlBoxHolder_,
        0, /* fromRow */
        0, /* fromCol */
        4, /* rowSpan - all content rows */
        1  /* colSpan */
        );

    // The box is a plain VIEWPORT: the layout keeps it at the holder's full
    // rect, it clips, and the heads inside carry the vertical scroll (see
    // layoutControlColumn). It must never be moved/resized by hand — the next
    // layout activation would silently undo that and unstick the heads from
    // the lanes. Its resize is where the heads get re-placed.
    qTrackControlBox_ = new QWidget();
    qTrackControlBox_->installEventFilter( this );
    trackHolderLayout->addWidget( qTrackControlBox_, 1 );  // sole occupant: takes the column

    // Track-reorder drag state + the insertion-line indicator (hidden until a
    // drag is in progress).
    dragControl_ = NULL;
    dropIndicator_ = new QFrame( qTrackControlBox_ );
    dropIndicator_->setStyleSheet( "background:#2080ff; border:none;" );
    dropIndicator_->hide();
//    qTrackControlBox_->setBackgroundMode( NoBackground );

    if( GLCOLSTRETCH_0>=0 ) qGridLayout_->setColumnStretch( 0, GLCOLSTRETCH_0 );
    if( GLCOLSTRETCH_1>=0 ) qGridLayout_->setColumnStretch( 1, GLCOLSTRETCH_1 );
    if( GLCOLSTRETCH_2>=0 ) qGridLayout_->setColumnStretch( 2, GLCOLSTRETCH_2 );
    if( GLCOLSTRETCH_3>=0 ) qGridLayout_->setColumnStretch( 3, GLCOLSTRETCH_3 );
    if( GLCOLSTRETCH_4>=0 ) qGridLayout_->setColumnStretch( 4, GLCOLSTRETCH_4 );

    if( GLROWSTRETCH_0>=0 ) qGridLayout_->setRowStretch( 0, GLROWSTRETCH_0 );
    if( GLROWSTRETCH_1>=0 ) qGridLayout_->setRowStretch( 1, GLROWSTRETCH_1 );
    if( GLROWSTRETCH_2>=0 ) qGridLayout_->setRowStretch( 2, GLROWSTRETCH_2 );
    if( GLROWSTRETCH_3>=0 ) qGridLayout_->setRowStretch( 3, GLROWSTRETCH_3 );

    qGridLayout_->addWidget( 
        qContent_,
        GLROWSTART_CONTENT,
        GLCOLSTART_CONTENT,
        GLROWSTOP_CONTENT-GLROWSTART_CONTENT+1,
        GLCOLSTOP_CONTENT-GLCOLSTART_CONTENT+1
        );
    // was: qGridLayout_->addMultiCellWidget( qContent_, GLROWSTART_CONTENT, GLROWSTOP_CONTENT, GLCOLSTART_CONTENT, GLCOLSTOP_CONTENT );
    qGridLayout_->addWidget( qVZoomOut_, GLROW_VZOOM_OUT, GLCOL_VZOOM_OUT );
    qGridLayout_->addWidget( qScrollVert_, GLROW_VSCROLL, GLCOL_VSCROLL );
    qGridLayout_->addWidget( qVZoomIn_, GLROW_VZOOM_IN, GLCOL_VZOOM_IN );
    qGridLayout_->addWidget( qHZoomOut_, GLROW_HZOOM_OUT, GLCOL_HZOOM_OUT );
    qGridLayout_->addWidget( qScrollHoriz_, GLROW_HSCROLL, GLCOL_HSCROLL );
    qGridLayout_->addWidget( qHZoomIn_, GLROW_HZOOM_IN, GLCOL_HZOOM_IN );
    qGridLayout_->addWidget( qZoomTotal_, GLROW_TOTAL_ZOOM, GLCOL_TOTAL_ZOOM );
    
    timeGridSpec_.setTimeGridWidth( 60./model_->getProject().getBPMTempo() );
    timeGridSpec_.setEmphasizeGrids( 0, 4 );
    timeGridSpec_.setEmphasizeGrids( 1, 0 );
    timeGridSpec_.setEmphasizeGrids( 2, 0 );
    timeGridSpec_.setEmphasizeGrids( 3, 0 );

    currentSnapSpec_ = new SSnapSpec( timeGridSpec_ );
    if( model_ ) {
        currentSnapSpec_->setSampleRate( model_->getProject().getSRate() );
        // THE tempo authority (proposal 37 D2). The snap spec converts a named
        // division through it, never through 60/bpm.
        currentSnapSpec_->setTempoMap( model_->getProject().tempoMap() );
    }

    QObject::connect( model_, SIGNAL( durationChanged( length_t ) ), 
                      this, SLOT( contentDurationChanged( length_t ) ) );
    QObject::connect( &(SApplication::app()), SIGNAL( globalLocatorMoved( offset_t, offset_t ) ),
                      qContent_, SLOT( globalLocatorMoved( offset_t, offset_t ) ) );
    // View-follows-playhead: only the poll-driven advance (never a manual seek).
    QObject::connect( &(SApplication::app()), SIGNAL( locatorAdvanced( offset_t, offset_t ) ),
                      qContent_, SLOT( followLocator( offset_t, offset_t ) ) );
    QObject::connect( model_, SIGNAL( trackInserted( int, STrack & ) ), 
                      SLOT( nTracksChanged() ) );
    QObject::connect( model_, SIGNAL( trackRemoved( int, STrack & ) ), 
                      SLOT( nTracksChanged() ) );
    QObject::connect( model_, SIGNAL( trackInserted( int, STrack & ) ),
                      SLOT( addMixerControl( int, STrack & ) ) );
    QObject::connect( model_, SIGNAL( trackRemoved( int, STrack & ) ),
                      SLOT( removeMixerControl( int, STrack & ) ) );
    QObject::connect( model_, SIGNAL( tracksReordered() ),
                      SLOT( tracksReordered() ) );
    QObject::connect( this, SIGNAL( timeGridSpecChanged( const STimeGridSpec & ) ), 
                      SLOT( update() ) );

    QObject::connect( qContent_, SIGNAL( leftOffsetChanged( offset_t ) ), 
                      this, SLOT( avLeftOffsetChanged( offset_t ) ) );

    QObject::connect( &(model_->getProject()), SIGNAL( bpmTempoChanged( double ) ),
                      this, SLOT( onProjectTempoChanged( double ) ) );

    // Take lanes: clip-level edits (add-take/remove-take/stack split) change
    // an expanded track's row count without a track-structure signal.
    QObject::connect( &(model_->getProject()), SIGNAL( arrangementChanged() ),
                      this, SLOT( onArrangementChangedRows() ) );

    // Persistent actions with keyboard shortcuts (active whenever the arranger
    // window is up). They are also placed in the right-click menu by ctGlobalShow.
    actNewTrack_ = new QAction( "&New track", this );
    actNewTrack_->setShortcut( Qt::CTRL | Qt::Key_T );
    QObject::connect( actNewTrack_, SIGNAL( triggered() ), this, SLOT( ctAddTrack() ) );
    addAction( actNewTrack_ );

    actInsertSample_ = new QAction( "&Insert sample", this );
    actInsertSample_->setShortcuts(
        { QKeySequence( Qt::CTRL | Qt::Key_Return ),
          QKeySequence( Qt::CTRL | Qt::Key_Enter ) } );
    QObject::connect( actInsertSample_, SIGNAL( triggered() ), this, SLOT( ctInsertSample() ) );
    addAction( actInsertSample_ );

    actSplit_ = new QAction( "&Split object", this );
    actSplit_->setShortcut( Qt::Key_S );
    QObject::connect( actSplit_, SIGNAL( triggered() ), this, SLOT( ctSplitSample() ) );
    addAction( actSplit_ );

    actRemoveSample_ = new QAction( "&Remove sample", this );
    actRemoveSample_->setShortcuts( { Qt::Key_Delete, Qt::Key_Backspace } );
    QObject::connect( actRemoveSample_, SIGNAL( triggered() ), this, SLOT( ctRemoveSample() ) );
    addAction( actRemoveSample_ );

    // Clip transposition (cents, realised in the grain stage — pitch never
    // moves a clip edge). The keypad bindings are the layout-proof ones: on a
    // US layout "+" IS Shift+"=", so the Shift variants of the main row are
    // ambiguous there, while on a German layout (where "+" is unshifted) both
    // rows work. The timeline's primary-modifier gestures (⌘ on macOS, Ctrl
    // elsewhere) are kept off these QAction shortcuts: the primary modifier +
    // drag is time-stretch / duplicate.
    auto addPitchAction = [&]( const QString &text, const QList<QKeySequence> &keys,
                               const char *slot ) -> QAction* {
        QAction *a = new QAction( text, this );
        a->setShortcuts( keys );
        // Scoped to the arranger (which takes click focus, see qContent_):
        // "+"/"-" are ordinary characters, so a window-wide shortcut would
        // hijack them from every text/number field in the main window.
        a->setShortcutContext( Qt::WidgetWithChildrenShortcut );
        QObject::connect( a, SIGNAL( triggered() ), this, slot );
        addAction( a );
        return a;
    };
    actPitchUp_ = addPitchAction(
        "Pitch &up (semitone)",
        { QKeySequence( Qt::Key_Plus ), QKeySequence( Qt::Key_Equal ),
          QKeySequence( Qt::KeypadModifier | Qt::Key_Plus ) },
        SLOT( ctPitchUp() ) );
    actPitchDown_ = addPitchAction(
        "Pitch &down (semitone)",
        { QKeySequence( Qt::Key_Minus ),
          QKeySequence( Qt::KeypadModifier | Qt::Key_Minus ) },
        SLOT( ctPitchDown() ) );
    actPitchUpFine_ = addPitchAction(
        "Pitch up (10 cents)",
        { QKeySequence( Qt::SHIFT | Qt::Key_Plus ),
          QKeySequence( Qt::ShiftModifier | Qt::KeypadModifier | Qt::Key_Plus ) },
        SLOT( ctPitchUpFine() ) );
    actPitchDownFine_ = addPitchAction(
        "Pitch down (10 cents)",
        { QKeySequence( Qt::SHIFT | Qt::Key_Minus ),
          QKeySequence( Qt::ShiftModifier | Qt::KeypadModifier | Qt::Key_Minus ) },
        SLOT( ctPitchDownFine() ) );
    actPitchUp_->setToolTip( "Transpose the selected clip(s) up one semitone "
                             "(Shift: 10 cents; numpad +/- on any layout)" );
    actPitchDown_->setToolTip( "Transpose the selected clip(s) down one semitone "
                              "(Shift: 10 cents; numpad +/- on any layout)" );

    // Build the flattened tree + control column for whatever already resides in
    // the mixer (refreshTrackTree handles rows, controls and scroll range).
    {
        refreshTrackTree();
        contentDurationChanged( model_->getDuration() );
        update();
    }

    // Create the draggable divider between track header and content BEFORE
    // restoring the saved width: setTrackControlWidth() sizes the grid column
    // to width + divider, and a setColumnMinimumWidth() afterwards would
    // clobber it.
    STrackHeaderResizer *resizer = new STrackHeaderResizer(this, this);
    qGridLayout_->addWidget(resizer, 0, 0, 4, 1, Qt::AlignRight);

    // Load saved track control width
    loadTrackControlWidth();

    // The track detail panel is NOT ours: it lives in a dock of the main window
    // (below the extern file list) and follows this mixer's selection from
    // there — see SMainWindow::attachTrackDetail(). All we still care about is
    // repainting the lanes when the selection highlight moves.
    connect(model_, &SStdMixer::selectedTrackChanged,
            qContent_, QOverload<>::of(&QWidget::update));
    // ...and when the SET changes without the primary moving (a Ctrl-click on
    // a third lane), which is exactly the case the lane tint exists for.
    connect(model_, &SStdMixer::selectedTracksChanged,
            qContent_, QOverload<>::of(&QWidget::update));
}

void SStdMixerView::setTrackControlWidth( int width )
{
    qWarning( "SStdMixerView::setTrackControlWidth( %d )", width );
    // Clamp between minimal and standard
    if( width < TRACK_CTRL_WIDTH_MINIMAL ) width = TRACK_CTRL_WIDTH_MINIMAL;
    if( width > TRACK_CTRL_WIDTH_STANDARD ) width = TRACK_CTRL_WIDTH_STANDARD;
    qWarning( "  -> clamped to: %d", width );

    const bool changed = ( trackControlWidth_ != width );
    trackControlWidth_ = width;

    // Applied unconditionally (all idempotent): the very first call comes from
    // loadTrackControlWidth() and usually restores the width we already hold,
    // and the column constraints still have to be established then.
    qTrackControlBoxHolder_->setMinimumWidth( width );
    qTrackControlBoxHolder_->setMaximumWidth( width );
    if( qGridLayout_ ) {
        qGridLayout_->setColumnStretch( 0, 0 );
        qGridLayout_->setColumnMinimumWidth( 0, width + 8 );   // +8 for divider
    }

    // Re-place (not re-create) the heads at the new width.
    layoutControlColumn();

    if( !changed ) return;

    // Trigger layout recalculation
    saveTrackControlWidth();
    if( qGridLayout_ ) {
        qGridLayout_->invalidate();
    }
    updateGeometry();
    update();
}

void SStdMixerView::saveTrackControlWidth()
{
    SSettings &settings = SSettings::instance();
    settings.setValue( "MixerView/TrackControlWidth", trackControlWidth_ );
}

void SStdMixerView::loadTrackControlWidth()
{
    SSettings &settings = SSettings::instance();
    int saved = settings.value( "MixerView/TrackControlWidth", TRACK_CTRL_WIDTH_MINIMAL ).toInt();
    setTrackControlWidth( saved );
}

void SStdMixerView::updateDividerCursor( const QPoint &pos )
{
    // Show resize cursor near the divider (within 5 pixels of track header right edge)
    int dividerX = trackControlWidth_;
    if( pos.x() >= dividerX - 5 && pos.x() <= dividerX + 5 ) {
        setCursor( Qt::SizeHorCursor );
    } else {
        setCursor( Qt::ArrowCursor );
    }
}

void SStdMixerView::mousePressEvent( QMouseEvent *event )
{
    // Check if clicking on the divider (resize handle)
    int dividerX = trackControlWidth_;
    if( event->button() == Qt::LeftButton &&
        event->position().toPoint().x() >= dividerX - 5 &&
        event->position().toPoint().x() <= dividerX + 5 ) {
        trackHeaderDragActive_ = true;
        trackHeaderDragStartX_ = event->position().toPoint().x();
        trackHeaderDragStartWidth_ = trackControlWidth_;
        event->accept();
        return;
    }
    QWidget::mousePressEvent( event );
}

void SStdMixerView::mouseMoveEvent( QMouseEvent *event )
{
    if( trackHeaderDragActive_ ) {
        int delta = event->position().toPoint().x() - trackHeaderDragStartX_;
        int newWidth = trackHeaderDragStartWidth_ + delta;
        setTrackControlWidth( newWidth );
        event->accept();
    } else {
        // Update cursor based on position
        updateDividerCursor( event->pos() );
        QWidget::mouseMoveEvent( event );
    }
}

void SStdMixerView::mouseReleaseEvent( QMouseEvent *event )
{
    if( trackHeaderDragActive_ && event->button() == Qt::LeftButton ) {
        trackHeaderDragActive_ = false;
        event->accept();
    } else {
        QWidget::mouseReleaseEvent( event );
    }
}

SStdMixerView::~SStdMixerView()
{
    delete currentSnapSpec_;
    delete controlArray_;
    delete autoUi_;
}

// Detail-editor registration (proposal 14, Phase 6): the model asks the
// factory; this UI module supplies the widget for SStdMixer.
#include "app/model/sdetaileditors.h"
static QWidget *createStdMixerEditor( SObject &obj, QWidget *parent )
{
    return new SStdMixerView( parent, static_cast<SStdMixer *>( &obj ) );
}
static const bool s_registered_mixereditor =
    ( sdetaileditors::registerEditor( "SStdMixer", createStdMixerEditor ), true );
