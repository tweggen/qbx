
#include <stdlib.h>

#include <algorithm>

#include <QtDebug>
#include <qwidget.h>
#include <qpushbutton.h>
#include <qevent.h>
#include <qpainter.h>
#include <qmenu.h>
#include <qfiledialog.h>
#include <QFileInfo>
#include <qscrollbar.h>
#include <qlayout.h>
#include <qmessagebox.h>
#include <qinputdialog.h>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QMainWindow>
#include <QStatusBar>
#include <QApplication>

#include "tw/sources/twwavinput.h"
#include "tw/playback/twspeaker.h"
#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/timeline/sstdmixerview.h"
#include "app/timeline/strackheaderresizer.h"
#include "app/objects/track/strack.h"
#include "app/model/sobjectrenderer.h"
#include "app/objects/wave/splainwave.h"
#include "app/model/slink.h"
#include "app/objects/cut/scut.h"
#include "app/objects/cut/swarpmarkeractions.h"
#include "app/model/sexternfile.h"
#include "app/objects/cut/scutrndrinline.h"   // loop-marker handle geometry
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
#include "app/objects/mixer/smovetrackaction.h"
#include "app/objects/mixer/sreparenttrackaction.h"
#include "app/objects/mixer/sremovetrackaction.h"
#include "app/objects/track/smoveclipaction.h"
#include "app/objects/cut/ssplitclipaction.h"
#include "app/objects/cut/sduplicateclipaction.h"
#include "app/objects/cut/sresizeclipaction.h"
#include "app/objects/mixer/screateassetaction.h"
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
#include "app/model/splacements.h"
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

// Vertical wheel-scroll: angleDelta units accumulated before stepping one track
// lane. A standard mouse notch is 120 units; at 600 that is one lane per 5 notches
// — ~1/5 the previous per-event sensitivity — and it also tames trackpad / Magic
// Mouse sub-notch deltas that used to jump a whole lane each.
static constexpr int SMV_WHEEL_VSCROLL_STEP = 600;

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
    // so the row geometry — and with it the scroll offset, which is a running
    // sum, not a multiple — has to be rebuilt before anything is placed.
    smv_.rebuildRowGeometry();
    upperLeftY_ = smv_.rowTop( (int) topRow_ );
    smv_.layoutControlColumn();
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
    topRow_ = clampTopRow( topOffset, smv_.rowCount() );
    upperLeftY_ = smv_.rowTop( (int) topRow_ );
    smv_.layoutControlColumn();
    setLeftOffset( leftOffset );
    // FIXME: Blitting
    // FIXME: Signal
    update();
}

void SMVActualView::setLeftOffset( offset_t leftOffset )
{   
    if( upperLeftOffset_ == leftOffset ) return;
    upperLeftOffset_ = leftOffset;
    int srate = smv_.model_ ? smv_.model_->getProject().getSRate() : 48000;
    int newUpperLeftX = ((int)((((double)leftOffset)/srate)*secondWidth_));
    if( upperLeftX_ == newUpperLeftX ) return;
    upperLeftX_ = newUpperLeftX;
//    qWarning( "SMVActualView::setLeftOffset(): leftOffset = %d:%d, upperLeftX_ = %d",
//              (int)leftOffset, (int)(leftOffset>>32), upperLeftX_ );
    // FIXME: Blitting
    emit leftOffsetChanged( leftOffset );
    update();
}

void SMVActualView::setTopOffset( idx_t topOffset )
{
    topRow_ = clampTopRow( topOffset, smv_.rowCount() );
    upperLeftY_ = smv_.rowTop( (int) topRow_ );
    smv_.layoutControlColumn();
    // FIXME: Blitting
    // FIXME: Signal
    update();
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

void SMVActualView::globalLocatorMoved( offset_t newPos, offset_t oldPos )
{
    // Qt6 forbids constructing a QPainter on a widget outside paintEvent.
    // Instead, invalidate 3px columns so paintEvent redraws the playhead (see the
    // cursor block at the end of paintEvent).
    QRect myRect = rect();
    int w = myRect.width();
    int h = myRect.height();
    int newX = getXPosOfOffset( newPos );
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

void SMVActualView::followLocator( offset_t newPos, offset_t oldPos )
{
    // Only when enabled, and only for a real advance under playback/recording
    // (this slot is wired to locatorAdvanced, so a manual seek never lands here).
    if( !followPlayhead_ ) return;
    if( newPos == oldPos ) return;

    int w = width();
    if( w <= 0 ) return;
    int x = getXPosOfOffset( newPos );   // cursor position in view-space pixels

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
        p.setPen( QColor( 96, 96, 96 ) );
        ctx.setVisibRect(
            QRect( 0, top+1, myRect.width(), lh-2 ) );
        p.drawLine( 0, top, myRect.bottomRight().x(), top );
        p.drawLine( 0, top+lh-1,
                    myRect.bottomRight().x(), top+lh-1 );
        if( row->isSubLane() ) {
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
                            QColor( 30, 46, 66 ) );
            }
            // Draw the track's clips.
            row->track->getInlineRenderer()->draw( *row->link, ctx );
        }
    }

    // While recording, show the in-progress capture as a translucent region that
    // grows with the playhead, on each armed lane (from the record start to the
    // live locator). The real clip — with its waveform — is placed when recording
    // finishes; this is live feedback that something is being captured.
    if( SApplication::app().isRecordingActive() ) {
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
            p.setPen( emph ? QColor( 96, 96, 96 ) : QColor( 160, 160, 160 ) );
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
        int x = getXPosOfOffset( SApplication::app().getGlobalLocatorPos() );
        if( x>=0 && x<myRect.width() ) {
            p.setPen( QColor( 30, 200, 30 ) );
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
    SLink *newLink = new SLink( oldLink->getSObject(), NULL );
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
        SApplication::app().submitAction(
            new SRemoveAssetPlacementAction( assetName, trackPath, clipIdx, timePos )
        );
        return;
    }

    // Submit the removal action (proper undo/redo support)
    SApplication::app().submitAction(
        new SRemoveSampleAction(trackPath, clipIdx, filePath, timePos)
    );
}

void SStdMixerView::ctDeleteSample()
{
}

void SStdMixerView::ctSplitSample()
{
    STrack *oldTrack = qContent_->getLastClickTrack();
    SLink *oldLink = qContent_->getLastClickSLink();
    if( !oldTrack || !oldLink ) {
        qWarning( "ctSplitSample called without object.\n" );
        return;
    }
    // Through the action so the split (and the implicit ensure-SCut) is undoable.
    QList<int> clipPath = strackpath::pathOf( model_, oldTrack );
    clipPath.append( oldTrack->indexOfChild( oldLink ) );
    offset_t splitTime = SApplication::app().getGlobalLocatorPos();
    qContent_->resetLastClickSLink();   // the link may be replaced by the split
    SApplication::app().submitAction( new SSplitClipAction( clipPath, splitTime ) );
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

    app.submitAction( composite );

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
        qGlobalPopup_->addSeparator();
    }
    if( lastClickTrack_ ) {
        qGlobalPopup_->addAction( smv_.actInsertSample_ );
        qGlobalPopup_->addAction( smv_.actRemoveSample_ );
        qGlobalPopup_->addAction( "Delete sample", &smv_, SLOT( ctDeleteSample() ) );
        qGlobalPopup_->addSeparator();
    }
    qGlobalPopup_->addAction( smv_.actNewTrack_ );
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
    SApplication::app().submitAction( new SAddTrackAction( -1 ) );
}

/**
 * Add a new track whose parent is the same container as the last visible lane's
 * track (the "track above" the blank space the user double-clicked). Always goes
 * through the undoable SAction system.
 *
 * When that parent is the root mixer the whole gesture is a single
 * SAddTrackAction. When it is a folder track we follow the ctGroupTrack()
 * pattern: append a top-level track, then reparent it under the folder — two
 * SActions wrapped in one undo macro so a single undo reverses the gesture.
 */
void SStdMixerView::ctAddTrackBelowLast()
{
    if( !model_ ) return;

    // "Track above" = the last lane in the flattened tree; its container is the
    // parent the new track should share. No lanes yet -> append to the mixer.
    const STrackRow *last = rowAt( rowCount() - 1 );
    SObject *parent = last ? last->parent : (SObject*)model_;
    STrack *parentTrack = dynamic_cast<STrack*>( parent );

    if( !parentTrack ) {
        // Parent is the root mixer: a plain append is undoable on its own.
        SApplication::app().submitAction( new SAddTrackAction( -1 ) );
        return;
    }

    // Nested parent: create at the top level, then move under the folder.
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    if( stack ) stack->beginMacro( "Add track" );
    SApplication::app().submitAction( new SAddTrackAction( -1 ) );
    // submitAction drains synchronously (Phase 1), so the new track is now the
    // last top-level track; reparent it under the target folder.
    int newIdx = model_->getNTracks() - 1;
    SApplication::app().submitAction( new SReparentTrackAction(
        QList<int>{ newIdx },
        strackpath::pathOf( model_, parentTrack ), -1 ) );
    if( stack ) stack->endMacro();
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
        SApplication::app().submitAction( new SRemoveTrackAction( path ) );
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
    SApplication::app().submitAction( new SReparentTrackAction(
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
    SApplication::app().submitAction( new SReparentTrackAction(
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
        SApplication::app().submitAction( new SAddTrackAction( ti ) );
    } else {
        // NESTED. add-track can only append at the MIXER's top level, and
        // SReparentTrackAction refuses a same-container move (that is
        // SMoveTrackAction's job) — so the folder cannot be born in place and
        // cannot be slid there afterwards if it starts as a sibling. Create it
        // top-level, then move it INTO the parent at the target's slot (a real
        // cross-container reparent), which pushes the targets down by one.
        SApplication::app().submitAction( new SAddTrackAction( -1 ) );
        const int folderTop = model_->getNTracks() - 1;   // append landed last
        SApplication::app().submitAction( new SReparentTrackAction(
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
            SApplication::app().submitAction( new SReparentTrackAction(
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
        SApplication::app().submitAction( new SReparentTrackAction(
            strackpath::pathOf( model_, k ), parentPath, insertAt ) );
        ++insertAt;
    }
    // Delete the now-empty folder (undoable: its restore brings it back, then the
    // child reparents undo back into it).
    const QList<int> fPath = strackpath::pathOf( model_, t );
    if( !fPath.isEmpty() ) {
        SApplication::app().submitAction( new SRemoveTrackAction( fPath ) );
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

void SMVActualView::updateLastClickVars( const QPoint &pos )
{
    lastClickedStart_ = lastClickedEnd_ = false;
    lastClickedEndUpper_ = false;
    lastClickedStartUpper_ = false;
    lastClickLoopMarker_ = 0;
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
    // re-creates it at its final (snapped) position. Several copies land as one
    // undo macro so the whole group reverts together.
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
        bool macro = fins.size() > 1 && stack;
        if( macro ) stack->beginMacro( QStringLiteral("Duplicate clips") );
        for( const Fin &f : fins ) {
            if( !f.src.isEmpty() && !f.dest.isEmpty() )
                SApplication::app().submitAction(
                    new SDuplicateClipAction( f.src, f.dest, f.start ) );
        }
        if( macro ) stack->endMacro();
        update();
        clipDragArmed_ = false;
        clipDragIsDuplicate_ = false;
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
        SCut *cut = dynamic_cast<SCut*>( &lastClickSLink_->getSObject() );
        if( cut ) {
            offset_t newStart   = lastClickSLink_->getStartTime();
            Fraction newAnchor  = cut->getSrcStart();
            length_t newDur     = cut->getDuration();
            length_t newLoop    = cut->getLoopLength().frames();
            Fraction newStretch = clipStretch0_;
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
                cut->processWindowParamEvents();

                // Then revert to pre-drag state and re-apply via action
                lastClickSLink_->setStartTime( clipDragStart0_ );
                cut->setWindow( clipSrcStart0_,
                                ClipLen( lastClickDuration_ ),
                                WarpedLen( clipLoopLen0_ ), clipStretch0_ );
                QList<int> clipPath = strackpath::pathOf( smv_.getModel(), lastClickTrack_ );
                clipPath.append( lastClickTrack_->indexOfChild( lastClickSLink_ ) );
                SApplication::app().submitAction(
                    new SResizeClipAction( clipPath, newStart, newAnchor, newDur,
                                           newLoop, newStretch ) );
                update();
            }
        }
        clipDragArmed_ = false;
        clipDragIsSlip_ = clipDragIsStretch_ = clipDragIsLoop_ = false;
        clipDragIsLoopMarker_ = clipDragIsLoopStart_ = false;
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
            SApplication::app().submitAction(
                new SMoveClipAction( clipPath, destTrackPath, newStart ) );
            update();
        }
    }
    clipDragArmed_ = false;

    // Reposition the cursor only when this click was NOT consumed by anything
    // else: no range/take/edge/marker gesture (those returned earlier), no clip
    // under the cursor (a clip click only selects), and no drag of any kind.
    // A pure click on empty timeline is the "unconsumed" case that seeks.
    if( rangeDrag_ == RangeNone && !clipDragArmed_ && lastClickSLink_ == NULL ) {
        // Check that the mouse didn't move significantly (within 4 pixels).
        // This distinguishes a click from a small drag (so we don't seek while
        // editing).
        const int CLICK_THRESHOLD = 4;
        QPoint delta = ev->pos() - lastClickPos_;
        if( delta.manhattanLength() <= CLICK_THRESHOLD ) {
            // No range drag and minimal mouse movement = pure click.
            offset_t ofs = smv_.alignTime( getTimeOf( ev->pos().x() ) );
            // setGlobalLocatorPos repositions the RUNNING engine too when
            // playing (see SApplication) — a plain model_->seekTo would only
            // move the component cursors, not the playback position.
            SApplication::app().setGlobalLocatorPos( ofs );
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
    p.fillRect( laneRect, QColor( 26, 38, 50 ) );   // darker than track lanes
    for( SLink *lk : row.track->childLinks() ) {
        STakeStack *stack = dynamic_cast<STakeStack*>( &lk->getSObject() );
        if( !stack ) continue;
        // Timeline invariant 2: the canvas does not know clip types. A take
        // lane draws whatever window is on it through the polymorphic renderer
        // path, so an event take paints like an audio one (proposal 37 P1) -
        // the SCut cast this replaced silently drew nothing.
        SObject *take = stack->takeObjectAt( row.takeRow );
        if( !take ) continue;                       // this stack has fewer takes
        const offset_t start = lk->getStartTime();
        const length_t dur = stack->getDuration();
        int x0 = getXPosOfOffset( start );
        int x1 = getXPosOfOffset( start + (offset_t)dur );
        if( x1 <= laneRect.left() || x0 >= laneRect.right() ) continue;
        if( x0 < laneRect.left() ) x0 = laneRect.left();
        if( x1 > laneRect.right() ) x1 = laneRect.right();
        QRect vr( x0, laneRect.y()+1, x1-x0, laneRect.height()-2 );
        if( vr.width() < 1 ) continue;

        const bool active = ( stack->activeTakeIndex() == row.takeRow );
        p.fillRect( vr, QColor( 160, 160, 160 ) );
        InlineRenderContext myctx( *this, p );
        myctx.setVisibRect( vr );
        if( SObjectRenderer *rndr = take->getInlineRenderer() )
            rndr->draw( *lk, myctx );   // outer link for timing, his own window
        if( active ) {
            p.setPen( QColor( 240, 220, 80 ) );
            p.drawRect( vr.adjusted( 0, 0, -1, -1 ) );
        } else {
            p.fillRect( vr, QColor( 0, 0, 0, 130 ) );   // dim inactive takes
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
        SApplication::app().submitAction( new SSetTempoAction( newTempo ) );
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
    // Feature (b): turn the right-clicked track + the ruler range into a reusable
    // live asset — an SCut windowing THAT track (vertical scope = the track and
    // its children), horizontal scope = the range. Scoping to a track (rather
    // than the whole mixer) is what lets a placement land on a sibling lane
    // without a self-reference cycle; placing it back inside the source track is
    // refused by the guard in SPlaceAssetAction. See
    // plan/proposed/05_TRACK_GROUPING_AND_LIVE_ASSETS.md feature (b) / §2.7.
    if( !rangeValid_ || !lastClickTrack_ ) return;
    offset_t t0 = getRangeStart();
    offset_t t1 = getRangeEnd();
    if( t1 <= t0 ) return;
    const QList<int> containerPath =
        strackpath::pathOf( smv_.model_, lastClickTrack_ );
    SApplication::app().submitAction(
        new SCreateAssetAction( containerPath, t0, (length_t)( t1 - t0 ) ) );
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
            if( loopMarkerAt( pos, rowIdx, clip ) > 0 )
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

/**
 * Mouse was moved. Look, if the left button currently is pressed, and if
 * an object was selected initially. If it was, move it.
 */
void SMVActualView::mouseMoveEvent( QMouseEvent *ev )
{
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

    // Check scrolling, if the event position is invisible.
    QRect myRect = rect();
    int srate = smv_.model_ ? smv_.model_->getProject().getSRate() : 48000;
    if( ev->pos().x()<0 ) {
        int currentOffset = upperLeftX_;
        int d = -ev->pos().x();
        currentOffset -= d;
        if( currentOffset<0 ) currentOffset = 0;
        if( currentOffset != upperLeftX_ ) {
            setLeftOffset( (offset_t)( ((double)currentOffset)/secondWidth_*srate) );
        }
    } else if( ev->pos().x()>=myRect.width() ) {
        int currentOffset = upperLeftX_;
        int d = ev->pos().x()-myRect.width();
        currentOffset += d;
        if( currentOffset != upperLeftX_ ) {
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
            offset_t downTime = getLastClickOffset();
            offset_t nowTime = getTimeOf( ev->pos().x() );
	    length_t delta = (length_t)nowTime-(length_t)downTime;
            length_t newStart = getLastClickStartOffset() + delta;
            if( newStart<0 ) newStart = 0;

            // Live drags below mutate only the fields needed for visual feedback
            // (cheap, no audio rebuild); the release reverts to the snapshot and
            // re-applies the whole window through SResizeClipAction.
            if( clipDragIsLoopMarker_ ) {
                // Drag a LOOP MARKER: re-tile the clip so the grabbed boundary
                // k lands under the pointer — segment = (t - clipStart)/k. The
                // clip's duration is untouched, so shortening the segment fits
                // more repetitions into the same clip. Kept strictly below the
                // duration so the clip stays looping (SCut::isLooping) and at
                // least one marker remains to grab.
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
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
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
                length_t contentLen = cut->getContent().hasDuration()
                                      ? (length_t) cut->getContent().getDuration() : -1;
                length_t d = (length_t) smv_.alignTime( getTimeOf( ev->pos().x() ) )
                           - (length_t) smv_.alignTime( (offset_t) getLastClickOffset() );
                length_t newOff = (length_t) clipResizeOffset0_ - d;
                // The slip MAY go negative (proposal 23): the clip then opens
                // with silence and its data starts later. Bounded so at least
                // SMV_CUT_MIN_TIME of content stays inside the clip — the mirror
                // of the maxOff rule below, which lets the tail run into silence.
                length_t minOff = SMV_CUT_MIN_TIME - (length_t) lastClickDuration_;
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
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
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
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
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
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
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
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
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
                    cut->setStartOffset( rCutStart );
                    cut->setDuration( rDur );
                    lastClickSLink_->setStartTime( rStart );
                    cut->invalidateCapture();  // Drop cached render, schedule async revalidation
                    // Non-blocking: get preview cache (or stale) for live feedback during drag
                    cut->getPreviewCapture();
                    smv_.getModel()->getProject().notifyArrangementChanged();  // Cascade to live assets
                    update( oldRect );
                    update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
                }
            } else if( lastClickedEnd_ ) {
                // Drag the RIGHT edge: set the duration to the snapped mouse time.
                // NOT clamped to the content length. A clip longer than its data
                // is a legitimate state, not an accident: a looping clip tiles
                // its segment for as long as you drag, and a plain one simply
                // runs into silence past the end of its sample — which is
                // exactly what "Remove loop" leaves behind, so that clip has to
                // survive a later extend too. The old clamp to
                // (contentLen - startOffset) snapped both back to about the
                // length of their data.
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
                offset_t rEnd = smv_.alignTime( getTimeOf( ev->pos().x() ) );
                length_t rDur = (length_t) rEnd - (length_t) clipDragStart0_;
                if( rDur < SMV_CUT_MIN_TIME ) rDur = SMV_CUT_MIN_TIME;
                QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                // Only rebuild if duration actually changed (not clamped to same value)
                if( rDur != cut->getDuration() ) {
                    cut->setDuration( rDur );
                    cut->queueWindowParamEvent( DURATION_CHANGE, (double) rDur );
                    cut->getPreviewCapture();  // Non-blocking: schedule async revalidation if needed
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
    SApplication::app().submitAction(
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
            SApplication::app().submitAction(
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
    SApplication::app().submitAction(
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
    SApplication::app().submitAction(
        new SAddWarpMarkerAction( path, src, warped ) );
    update();
    return true;
}

void SMVActualView::mouseDoubleClickEvent( QMouseEvent *ev )
{
    // W2: double-click in a clip's marker strip adds a warp marker.
    if( ev->button() == Qt::LeftButton && smv_.getModel()
        && tryAddMarkerAt( ev ) )
        return;
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

    // Take-lane rows: a left click on a take ACTIVATES it — the comping
    // gesture (proposal 17 phase 3, undoable select-take). Take lanes host no
    // other gestures yet, so the click is consumed either way.
    if( ev->buttons() & Qt::LeftButton ) {
        const STrackRow *clickRow = smv_.rowAt( lastClickTrackIdx_ );
        if( clickRow && clickRow->takeRow >= 0 ) {
            if( lastClickSLink_ ) {
                STakeStack *stack = dynamic_cast<STakeStack*>(
                    &lastClickSLink_->getSObject() );
                if( stack && stack->takeAt( clickRow->takeRow ) ) {
                    QList<int> path =
                        strackpath::pathOf( smv_.getModel(), clickRow->track );
                    path.append(
                        clickRow->track->indexOfChild( lastClickSLink_ ) );
                    SApplication::app().submitAction(
                        new SSelectTakeAction( path, clickRow->takeRow ) );
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
                        SApplication::app().submitSetSelectionAction( anchorCopy );
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
                    clipDragIsLoopMarker_ = ( lastClickLoopMarker_ > 0 );
                    clipDragIsSlip_    = ( alt && !onBorder
                                           && !clipDragIsLoopMarker_ );
                    clipDragIsStretch_ = ( hasPrimaryMod( modifiers ) && onBorder );
                    clipDragIsLoop_    = ( !hasPrimaryMod( modifiers )
                                           && lastClickedEnd_ && lastClickedEndUpper_ );
                    clipDragIsLoopStart_ = ( !hasPrimaryMod( modifiers )
                                             && lastClickedStart_ && lastClickedStartUpper_ );
                    clipLoopSeg_ = 0;   // captured lazily on the first loop move
                    clipDragTrack0_ = lastClickTrack_;
                    clipDragStart0_ = lastClickSLink_->getStartTime();
                    {
                        SObject &o = lastClickSLink_->getSObject();
                        if( qstrcmp( o.metaObject()->className(), "SCut" ) == 0 ) {
                            SCut *c = (SCut*)&o;
                            clipResizeOffset0_ = (offset_t) c->getStartOffset().frames();
                            clipSrcStart0_     = c->getSrcStart();
                            clipLoopLen0_      = c->getLoopLength().frames();
                            clipStretch0_ = c->getStretchExact();
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

// True if a container has at least one child that is itself a track (so it is a
// foldable parent in the arranger).
static bool hasChildTracks( SObject *container )
{
    for( SLink *lk : container->childLinks() ) {
        if( dynamic_cast<STrack*>( &lk->getSObject() ) ) return true;
    }
    return false;
}

// The take-lane row count of a track: the widest take stack among its clips
// (proposal 17 phase 3). 0 = no stacks, nothing to expand.
static int maxTakesOf( STrack *tk )
{
    int maxTakes = 0;
    for( SLink *lk : tk->childLinks() ) {
        if( STakeStack *stack =
                dynamic_cast<STakeStack*>( &lk->getSObject() ) ) {
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
        bool kids = hasChildTracks( tk );
        bool col = collapsed_.contains( tk );
        rows_.append( STrackRow{ tk, lk, container, depth, kids, col } );
        // Take lanes directly below the track's composite lane.
        if( takesExpanded_.contains( tk ) ) {
            const int mt = maxTakesOf( tk );
            for( int k = 0; k < mt; ++k ) {
                rows_.append( STrackRow{ tk, lk, container, depth,
                                         false, false, k } );
            }
        }
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
    rebuildRows();
    if( rows_.size() != before ) {
        rebuildControlColumn();
        nTracksChanged();
        qContent_->setTopOffset( qContent_->getTopRow() );
    } else {
        // Same lane count, but a take could have moved between tracks — the
        // heads are cheap to re-place and must not be left behind.
        layoutControlColumn();
    }
    qContent_->update();
}

void SStdMixerView::rebuildRows()
{
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
    // re-anchor the scroll on the same row and replace the heads.
    rebuildRowGeometry();
    qContent_->setTopOffset( qContent_->getTopRow() );
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

int SStdMixerView::visibleRowCountFrom( int firstRow ) const
{
    int avail = qContent_->height() - SMV_TIME_RULER_HEIGHT;
    int n = 0;
    for( int i=qMax( 0, firstRow ); i<rows_.size() && avail>0; ++i ) {
        avail -= rowHeight( i );
        ++n;
    }
    return qMax( 1, n );
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
void SStdMixerView::layoutControlColumn()
{
    if( !qTrackControlBox_ || !controlArray_ ) return;
    const int w = trackControlWidth_;
    for( int c=0; c<controlArray_->size(); ++c ) {
        SSMVMixerControl *mc = controlArray_->at( c );
        if( !mc ) continue;
        const int row = ( c < controlRow_.size() ) ? controlRow_.at( c ) : c;
        mc->setGeometry( 0, controlYOfRow( row ), w, laneGroupHeight( row ) );
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
    if( collapsed_.contains( t ) ) collapsed_.remove( t );
    else                           collapsed_.insert( t );
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
    rebuildRows();
    rebuildControlColumn();
    nTracksChanged();
    // Rows moved under the scroll anchor: re-derive the pixel offset from the
    // (clamped) top row, which also re-places the heads.
    qContent_->setTopOffset( qContent_->getTopRow() );
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
            SApplication::app().submitAction( new SReparentTrackAction(
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
                SApplication::app().submitAction(
                    new SMoveTrackAction( QList<int>{ fromTop }, target ) );
            slot = target + 1;      // the next one goes just below this one
        } else {
            int target = slot;
            if( target<0 ) target = 0;
            if( target>nTop ) target = nTop;
            SApplication::app().submitAction( new SReparentTrackAction(
                strackpath::pathOf( model_, t ), QList<int>{}, target ) );
            slot = target + 1;
        }
    }
    if( macro ) stack->endMacro();
}

void SStdMixerView::nTracksChanged()
{
    int newNTracks = rowCount();    // visible lanes, not just top-level tracks
    int currValue = qScrollVert_->value();
    int pageStep = qScrollVert_->pageStep();
    if( currValue+pageStep > newNTracks ) {
        currValue = newNTracks-pageStep;
        if( currValue<0 ) currValue = 0;
        trackSliderMoved( currValue );
    }
    qScrollVert_->setMaximum( qMax( 0, (int) newNTracks-pageStep ) );
}

void SStdMixerView::avLeftOffsetChanged( offset_t newValue )
{
    int sliderValue;
    offset_t dur = (offset_t) 1;
    if( model_->hasDuration() ) {
	dur = model_->getDuration();
    }
    sliderValue = (dur + HSliderRange * newValue) / dur;
    // Correct the scroll bar.
    if( sliderValue != (int)qScrollHoriz_->value() ) qScrollHoriz_->setValue( sliderValue );
}

void SStdMixerView::timeSliderMoved( int newValue )
{
    if( newValue<0 ) {
	   //qWarning( "SStdMixerView::timeSliderMoved(): newValue was less than zero." );
	   newValue = 0;
    }
    //qWarning( "SStdMixerView::timeSliderMoved(): newValue=%d.",
	//      newValue );
    if( model_->hasDuration() ) {
	   qContent_->setLeftOffset( (offset_t)(newValue*model_->getDuration()/HSliderRange+0.5) );
    } else {	
	   qContent_->setLeftOffset( 0 );
    }
}

void SStdMixerView::trackSliderMoved( int newValue )
{
    if( newValue<0 ) {
	   qWarning( "SStdMixerView::trackSliderMoved(): newValue was less than zero." );
	   newValue = 0;
    }
    qContent_->setTopOffset( newValue );
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
bool SStdMixerView::dragClipEdge( int rowIdx, int clipIdx, int grabWhere,
                                  offset_t dropTime, bool upperHalf,
                                  Qt::KeyboardModifiers mods )
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
    int x0 = ( grabWhere == GrabEnd )  ? ex - 1
           : ( grabWhere == GrabBody ) ? ( sx + ex ) / 2
                                       : sx;
    if( grabWhere == GrabBody && ( x0 < sx + SMV_LEFT_DRAG_PIXEL
                                   || x0 >= ex - SMV_RIGHT_DRAG_PIXEL ) )
        return false;   // clip too narrow to have a body clear of the edge bands
    int x1 = qContent_->getXPosOfOffset( dropTime );
    if( x0 < 0 || x1 < 0 ) return false;

    int th = rowHeight( rowIdx );
    int laneTop = qContent_->laneTop( rowIdx );
    int y = laneTop + ( upperHalf ? th/4 : (3*th)/4 );

    // The drag auto-scrolls when the pointer leaves the canvas, which would move
    // the time axis mid-gesture. The window is never shown in test mode, so grow
    // the canvas until both ends are inside it and the drag stays a pure edit.
    int needed = ( x0 > x1 ? x0 : x1 ) + 64;
    if( qContent_->width() < needed )
        qContent_->resize( needed, qContent_->height() > y+th ? qContent_->height() : y+th+64 );

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

// --- test entry points for the lane geometry ----------------------------

void SStdMixerView::tkSetBaseTrackHeight( int h )
{
    qContent_->setTrackHeight( h );
}

void SStdMixerView::tkSetTopRow( int row )
{
    qContent_->setTopOffset( row );
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
    //    exactly on the lane the canvas paints for it.
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
        const QRect want( 0, qContent_->laneTop( i ), trackControlWidth_,
                          laneGroupHeight( i ) );
        if( g != want )
            return QString( "head %1 (row %2): geometry %3,%4 %5x%6, "
                            "lane is %7,%8 %9x%10" )
                       .arg( c ).arg( i )
                       .arg( g.x() ).arg( g.y() ).arg( g.width() ).arg( g.height() )
                       .arg( want.x() ).arg( want.y() )
                       .arg( want.width() ).arg( want.height() );
        ++c;
    }
    if( c != controlArray_->size() )
        return QString( "%1 heads for %2 track lanes" )
                   .arg( controlArray_->size() ).arg( c );
    return QString();
}

SLink *SStdMixerView::ensureSCut( SLink *lk )
{
    if( !lk ) return NULL;
    SObject *so = &(lk->getSObject());
    if( !qstrcmp( so->metaObject()->className(), "SCut" ) ) {
        // Not needed to create an scut.
        return lk;
    }
    qWarning( "Class name is %s and not SCut, so creating a new SCut object.\n",
              so->metaObject()->className() );
    offset_t oldStart = lk->getStartTime();
    SObject *pso = (SObject *)lk->parent();
    SCut *sc = new SCut( (SProject *)(so->parent()), *so );
    SLink *nlk = new SLink( *sc );
    nlk->setStartTime( oldStart );
    delete lk;
    nlk->setParent(pso); // was: pso->insertChild( nlk );
    return nlk;
}

void SStdMixerView::recalcPageStep()
{
    // qContent_ was resized. Recalc scrollbars.
    int w = qContent_->width();
    // Calc new pageStep.
    int srate = model_ ? model_->getProject().getSRate() : 48000;
    double dw = w;
    dw = dw * srate / qContent_->getSecondWidth();
    offset_t lw = (offset_t) (dw);
    if( lw>0x7fffffff ) lw = 0x7fffffff;
    offset_t dur = 1;
    if( model_->hasDuration() ) {
	dur = model_->getDuration();
    }
    dw = HSliderRange*(double)w/qContent_->getSecondWidth()*srate / (double)dur;
    int ps = (int)(dw+0.5);
    qScrollHoriz_->setPageStep( ps );
    qScrollHoriz_->setSingleStep( (ps/10)+1 );
    qScrollHoriz_->setMaximum( qMax( 0, (int)HSliderRange - ps ) );
    // Vertical scrolling stays row-granular, but with variable lane heights
    // "how many rows fit" depends on which rows are on screen.
    int vis = visibleRowCountFrom( (int) qContent_->getTopRow() );
    qScrollVert_->setPageStep( vis );
    qScrollVert_->setMaximum( qMax( 0, rowCount()-vis ) );
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

    trackHeight_ = 100;      // BASE lane height; per-track scales ride on it
    secondWidth_ = 30.;
    upperLeftX_ = upperLeftY_ = 0;
    topRow_ = 0;

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
        // Accumulate sub-notch deltas and step one lane per SMV_WHEEL_VSCROLL_STEP
        // units, so a trackpad / Magic Mouse no longer jumps a whole lane per event
        // (~1/5 the old sensitivity). +delta = wheel up = scroll toward upper rows.
        if( smv_.qScrollVert_ ) {
            wheelVScrollAccum_ += dy;
            int lanes = wheelVScrollAccum_ / SMV_WHEEL_VSCROLL_STEP;
            if( lanes != 0 ) {
                wheelVScrollAccum_ -= lanes * SMV_WHEEL_VSCROLL_STEP;
                smv_.qScrollVert_->setValue( smv_.qScrollVert_->value() - lanes );
            }
        }
        break;
    }

    case SOpt::ScrollHorizontal: {
        // Pan the timeline by ~1/8 of the visible span per notch.
        offset_t span = getTimeOf( width() ) - getTimeOf( 0 );
        offset_t step = span / 8;
        if( step < 1 ) step = 1;
        offset_t cur = upperLeftOffset_;
        offset_t next = (dir > 0) ? ( cur > step ? cur - step : 0 )   // up = earlier
                                  : cur + step;
        setLeftOffset( next );
        break;
    }

    case SOpt::ZoomHorizontal: {
        bool in = (dir > 0);
        if( wheelInvertZoom_ ) in = !in;
        double newW = secondWidth_ * ( in ? 1.2 : 1.0 / 1.2 );
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
        int h = in ? (trackHeight_ * 3) / 2 : (trackHeight_ * 2) / 3;
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

    // Get the target track from the row.
    const STrackRow *row = smv_.rowAt(rowIdx);
    if (!row || !row->track) {
        return;
    }

    STrack *track = row->track;
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
        SApplication::app().submitAction(new SPlaceAssetAction(assetName, trackPath, timePos));
    } else if (payload.startsWith(QStringLiteral("file:"))) {
        QString filePath = payload.mid(5);
        // For file drops, use SAddSampleAction (same as Insert Sample dialog).
        // The same trackPath the asset branch above uses: an index-path, so a
        // drop onto a track nested in a folder lands too. This was
        // mixer->indexOfChildObject(), which only sees top-level children and
        // made a drop onto a grouped lane a silent no-op.
        SApplication::app().submitAction(new SAddSampleAction(trackPath, filePath, timePos));
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
        /* 0, HSliderRange-1, HSliderRange, 
		  HSliderRange/10, 0, */ 
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
