
#include <stdlib.h>

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

#include "twwavinput.h"
#include "twspeaker.h"
#include "sapplication.h"
#include "sstdmixer.h"
#include "sstdmixerview.h"
#include "strackdetailpanel.h"
#include "strack.h"
#include "sobjectrenderer.h"
#include "splainwave.h"
#include "slink.h"
#include "scut.h"
#include "sproject.h"
#include "sprojectprops.h"
#include "ssettings.h"
#include "soptions.h"
#include <QWheelEvent>
#include <QCursor>
#include <QPixmap>
#include <QPolygon>
#include <QPen>
#include "ssmvmixercontrol.h"
#include "actions/saddtrackaction.h"
#include "actions/smovetrackaction.h"
#include "actions/sreparenttrackaction.h"
#include "actions/sremovetrackaction.h"
#include "actions/smoveclipaction.h"
#include "actions/ssplitclipaction.h"
#include "actions/sduplicateclipaction.h"
#include "actions/sresizeclipaction.h"
#include "actions/screateassetaction.h"
#include "actions/splaceassetaction.h"
#include "actions/saddsampleaction.h"
#include "actions/sremovesampleaction.h"
#include "actions/strackpath.h"
#include "sactionhistory.h"
#include <QFrame>
#include <QUndoStack>
#include <qaction.h>
#include <QKeySequence>

// Icons needed here

#include "pix/zoomin.xpm"
#include "pix/zoomout.xpm"

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
    // FIXME: Emit signal?
}

void SMVActualView::setTrackHeight( int h )
{
    if( h<6 ) h = 6;
    trackHeight_ = h;
    QVector<SSMVMixerControl*> &controls = *smv_.controlArray_;
    for( int t=0; t<controls.size(); t++ ) {
        SSMVMixerControl *mc = controls.at( t );
        if( !mc ) continue;
        mc->move( 0, getTrackHeight()*t );
        mc->setFixedSize( SMV_TRACK_CTRL_WIDTH, getTrackHeight() );
        mc->resize( SMV_TRACK_CTRL_WIDTH, getTrackHeight() );
    }
//    update();
    smv_.viewResized();
    update();
    // FIXME: Emit signal?
}

void SMVActualView::setUpperLeft( offset_t leftOffset, idx_t topOffset )
{
    int nTracks = smv_.rowCount();
    if( !nTracks ) {
        topOffset = 0;
    } else {
        if( topOffset<0 ) {
            topOffset = 0;
        } else if( topOffset>= nTracks ) {
            topOffset = nTracks - 1;
        }
    }
    upperLeftY_ = topOffset*trackHeight_;
    smv_.qTrackControlBox_->move( 0, -upperLeftY_+SMV_TIME_RULER_HEIGHT );
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
    int nTracks = smv_.rowCount();
    if( !nTracks ) {
        topOffset = 0;
    } else {
        if( topOffset<0 ) {
            topOffset = 0;
        } else if( topOffset>= nTracks ) {
            topOffset = nTracks - 1;
        }
    }
    upperLeftY_ = topOffset*trackHeight_;
    smv_.qTrackControlBox_->move( 0, -upperLeftY_+SMV_TIME_RULER_HEIGHT );
    // FIXME: Blitting
    // FIXME: Signal    
    update();
}

int SMVActualView::getXPosOfOffset( offset_t off ) const
{
    int srate = smv_.model_ ? smv_.model_->getProject().getSRate() : 48000;
    return ((int)((((double)off)/srate)*secondWidth_))-upperLeftX_;
}

void SMVActualView::globalLocatorMoved( offset_t newPos, offset_t oldPos )
{
    // Qt6 forbids constructing a QPainter on a widget outside paintEvent.
    // Instead, invalidate the columns around the old and new playhead positions
    // with a width of 3 pixels to ensure complete redraw (covers XOR artifacts).
    // paintEvent already knows how to redraw the playhead (see the cursor block
    // at the end of paintEvent).
    QRect myRect = rect();
    int w = myRect.width();
    int h = myRect.height();
    int oldX = getXPosOfOffset( oldPos );
    int newX = getXPosOfOffset( newPos );
    if( oldX == newX ) return;

    const int cursorWidth = 3;
    if( oldX >= 0 && oldX < w ) update( oldX - 1, 0, cursorWidth, h );
    if( newX >= 0 && newX < w ) update( newX - 1, 0, cursorWidth, h );

    // While recording, repaint the whole span the playhead swept so the growing
    // capture region fills in continuously (the 3px cursor columns alone would
    // leave gaps when zoomed in / moving fast).
    if( SApplication::app().isRecordingActive() ) {
        int lo = ( oldX < newX ? oldX : newX ) - 1;
        int hi = ( oldX < newX ? newX : oldX ) + 1;
        if( lo < 0 ) lo = 0;
        update( lo, 0, ( hi - lo ) + cursorWidth, h );
    }
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
    // OK, we have tracks (lanes of the flattened tree).
    int nTracks = smv_.rowCount();
    int firstTrack = (upperLeftY_ + trackHeight_-1) / trackHeight_;
    for( int i=firstTrack; i<nTracks; i++ ) {
        const STrackRow *row = smv_.rowAt( i );
        if( !row ) continue;
        int laneTop = SMV_TIME_RULER_HEIGHT+i*trackHeight_-upperLeftY_;
        // All lanes start at x=0 (full width) — the hierarchy is shown by the
        // indented control strips, not the timeline, so editing keeps full width.
        p.setPen( QColor( 96, 96, 96 ) );
        ctx.setVisibRect(
            QRect( 0, laneTop+1, myRect.width(), trackHeight_-2 ) );
        p.drawLine( 0, laneTop, myRect.bottomRight().x(), laneTop );
        p.drawLine( 0, laneTop+trackHeight_-1,
                    myRect.bottomRight().x(), laneTop+trackHeight_-1 );
        // Draw the track's clips.
        row->track->getInlineRenderer()->draw( *row->link, ctx );
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
                const STrackRow *row = smv_.rowAt( i );
                if( !row || !row->track || !row->track->isArmedForRecording() ) continue;
                int laneTop = SMV_TIME_RULER_HEIGHT+i*trackHeight_-upperLeftY_;
                p.fillRect( QRect( xs, laneTop+1, xe-xs, trackHeight_-2 ),
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
            p.drawLine( x, SMV_TIME_RULER_HEIGHT, x, SMV_TIME_RULER_HEIGHT+nTracks*trackHeight_-upperLeftY_ );
            a += tgs.getTimeGridWidth();
        }
    }
    int tmp = (SMV_TIME_RULER_HEIGHT+nTracks*trackHeight_-upperLeftY_);
    if( myRect.height()>tmp ) {        
        p.fillRect( QRect( 0, tmp, myRect.width(), myRect.height()-tmp+1 ), QColor( 0, 0, 0 ) );
    }
    // After painting all that track stuff, we try to paint the cursor.
    // Look, if the cursor is visible. As we are clipped to our range,
    // we safely can assume no cursor is there.
    {
        int x = getXPosOfOffset( SApplication::app().getGlobalLocatorPos() );
        if( x>=0 && x<myRect.width() ) {
            QPainter::CompositionMode oldCompositionMode = p.compositionMode();
            p.setCompositionMode( QPainter::CompositionMode_Xor );
            p.setPen( QColor( 30, 200, 30 ) );
            p.drawLine( x, 0, x, myRect.height()-1 );
            p.setCompositionMode( oldCompositionMode );
        }
    }

    // Time-range selection on top of everything (grey band in the ruler +
    // vertical edges over all tracks).
    drawRange( p, myRect );
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
                       "Wave files (*.WAV *.wav)");
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setOptions(QFileDialog::DontUseNativeDialog);
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
    SCut *soCut = new SCut( SApplication::app().getCurrentProject(), *lk );
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

    SObject *root = project->getRootComponent();
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
    if( !mixer ) return;

    // Get the track index in the mixer (top-level children)
    int trackIdx = mixer->indexOfChildObject(*oldTrack);
    if( trackIdx < 0 ) return;

    // Get the clip index within the track
    int clipIdx = oldTrack->indexOfChild(oldLink);
    if( clipIdx < 0 ) return;

    // Extract file path from the sample (empty if not available; the inverse action
    // can reconstruct from position info alone for undo/redo)
    QString filePath;
    SObject &obj = oldLink->getSObject();
    if( dynamic_cast<SCut*>(&obj) ) {
        // SCut wrapping a file link; file path not easily extractable, so leave empty
        // The SRemoveSampleAction inverse will restore based on track/clip index and time
        filePath = "";
    }

    offset_t timePos = oldLink->getStartTime();
    qContent_->resetLastClickSLink();

    // Submit the removal action (proper undo/redo support)
    SApplication::app().submitAction(
        new SRemoveSampleAction(trackIdx, clipIdx, filePath, timePos)
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

void SMVActualView::ctGlobalShow()
{
    qGlobalPopup_->clear();
    if( lastClickSLink_ ) {
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
        qGlobalPopup_->addAction( "Remove track", &smv_, SLOT( ctRemoveTrack() ) );
        qGlobalPopup_->addSeparator();
        qGlobalPopup_->addAction( "Indent track (nest under above)", &smv_, SLOT( ctIndentTrack() ) );
        qGlobalPopup_->addAction( "Outdent track", &smv_, SLOT( ctOutdentTrack() ) );
        qGlobalPopup_->addAction( "Group track", &smv_, SLOT( ctGroupTrack() ) );
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
// last track head) and turn them into a new-track gesture.
bool SStdMixerView::eventFilter( QObject *watched, QEvent *event )
{
    if( watched == qTrackControlBoxHolder_
        && event->type() == QEvent::MouseButtonDblClick ) {
        QMouseEvent *me = static_cast<QMouseEvent*>( event );
        if( me->button() == Qt::LeftButton ) {
            ctAddTrackBelowLast();
            return true;   // consumed
        }
    }
    return QWidget::eventFilter( watched, event );
}

/**
 * Remove the currently selected track. This also will remove its children.
 */
void SStdMixerView::ctRemoveTrack()
{
    // getLastClickTrackIdx() is a row index now; resolve the track and its real
    // mixer index. (Removing a nested track is not wired here yet.)
    STrack *t = qContent_->getLastClickTrack();
    if( !t || !model_ ) return;
    int idx = model_->indexOfChildObject( *t );
    if( idx<0 ) return;                 // nested track: not handled here
    // Through the action so it is undoable (the track + subtree is restorable).
    SApplication::app().submitAction( new SRemoveTrackAction( idx ) );
}

// --- grouping (proposal 05 §1.2) ----------------------------------------

void SStdMixerView::ctIndentTrack()
{
    STrack *t = qContent_->getLastClickTrack();
    if( !t || !model_ ) return;
    int ri = rowIndexOfTrack( t );
    if( ri<0 ) return;
    const STrackRow *row = rowAt( ri );
    SObject *parent = row->parent;
    int depth = row->depth;
    // Preceding sibling = the nearest earlier row sharing this parent.
    STrack *prevSibling = NULL;
    for( int i=ri-1; i>=0; --i ) {
        const STrackRow *r = rowAt( i );
        if( r->depth < depth ) break;          // left this sibling group
        if( r->parent == parent ) { prevSibling = r->track; break; }
    }
    if( !prevSibling ) return;                  // nothing to nest under
    SApplication::app().submitAction( new SReparentTrackAction(
        strackpath::pathOf( model_, t ),
        strackpath::pathOf( model_, prevSibling ), -1 ) );
}

void SStdMixerView::ctOutdentTrack()
{
    STrack *t = qContent_->getLastClickTrack();
    if( !t || !model_ ) return;
    int ri = rowIndexOfTrack( t );
    if( ri<0 ) return;
    SObject *parent = rowAt( ri )->parent;
    STrack *parentTrack = dynamic_cast<STrack*>( parent );
    if( !parentTrack ) return;                  // already top-level
    int pri = rowIndexOfTrack( parentTrack );
    SObject *grand = (pri>=0) ? rowAt( pri )->parent : (SObject*)model_;
    int dstIndex = grand->indexOfChildObject( *parentTrack ) + 1;  // just after parent
    SApplication::app().submitAction( new SReparentTrackAction(
        strackpath::pathOf( model_, t ),
        strackpath::pathOf( model_, grand ), dstIndex ) );
}

void SStdMixerView::ctGroupTrack()
{
    STrack *t = qContent_->getLastClickTrack();
    if( !t || !model_ ) return;
    int c = model_->indexOfChildObject( *t );   // top-level tracks only for now
    if( c<0 ) return;
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    if( stack ) stack->beginMacro( "Group track" );
    // New empty folder at T's slot; T then sits at c+1 and is moved into it.
    SApplication::app().submitAction( new SAddTrackAction( c ) );
    SApplication::app().submitAction( new SReparentTrackAction(
        QList<int>{ c+1 }, QList<int>{ c }, -1 ) );
    if( stack ) stack->endMacro();
}

void SStdMixerView::ctUngroupTrack()
{
    STrack *t = qContent_->getLastClickTrack();
    if( !t || !model_ ) return;
    int c = model_->indexOfChildObject( *t );   // top-level folders only for now
    if( c<0 ) return;
    QList<STrack*> kids;
    for( SLink *lk : t->childLinks() ) {
        if( STrack *k = dynamic_cast<STrack*>( &lk->getSObject() ) ) kids.append( k );
    }
    if( kids.isEmpty() ) return;
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    if( stack ) stack->beginMacro( "Ungroup track" );
    // Move each child out to the mixer, filling the slots just before the folder
    // so they end up where the folder was, in order. The empty folder is left in
    // place (an undoable track-remove is a separate task).
    int insertAt = c;
    for( STrack *k : kids ) {
        SApplication::app().submitAction( new SReparentTrackAction(
            strackpath::pathOf( model_, k ), QList<int>{}, insertAt ) );
        ++insertAt;
    }
    // Delete the now-empty folder (undoable: its restore brings it back, then the
    // child reparents undo back into it).
    int fIdx = model_->indexOfChildObject( *t );
    if( fIdx>=0 ) {
        SApplication::app().submitAction( new SRemoveTrackAction( fIdx ) );
    }
    if( stack ) stack->endMacro();
}

offset_t SMVActualView::getTimeOf( int x ) const
{
    offset_t totalX = x + getUpperLeftX();
    // Use actual project sample rate, not hardcoded 44100
    int sampleRate = smv_.model_->getProject().getSRate();
    double a = ((double)totalX)/getSecondWidth() * sampleRate;
    return (offset_t)a;
}

void SMVActualView::updateLastClickVars( const QPoint &pos )
{
    lastClickedStart_ = lastClickedEnd_ = false;
    lastClickedEndUpper_ = false;
    lastClickPos_ = pos;
    int y = pos.y()-SMV_TIME_RULER_HEIGHT;
    if( y<0 ) y = 0;
    lastClickTrackIdx_ = (y+upperLeftY_)/trackHeight_;
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
                }
                if( lastClickSLink_->getSObject().hasDuration() ) {
                    length_t len = lastClickSLink_->getSObject().getDuration();
                    lastClickDuration_ = len;
                    int endX = getXPosOfOffset( pos+len );
                    if( lastClickPos_.x() < endX
                        && lastClickPos_.x() >= (endX-SMV_RIGHT_DRAG_PIXEL) ) {
                        lastClickedEnd_ = true;
                        // Upper half of the lane → loop; lower half → extend.
                        int laneY = ( y + upperLeftY_ ) % trackHeight_;
                        lastClickedEndUpper_ = ( laneY < trackHeight_/2 );
                    }
                }
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
             || clipDragIsStretch_ || clipDragIsLoop_ ) ) {
        SCut *cut = dynamic_cast<SCut*>( &lastClickSLink_->getSObject() );
        if( cut ) {
            offset_t newStart   = lastClickSLink_->getStartTime();
            offset_t newOffset  = cut->getStartOffset();
            length_t newDur     = cut->getDuration();
            length_t newLoop    = cut->getLoopLength();
            double   newStretch = clipStretch0_;
            if( clipDragIsStretch_ ) {
                // The grain view is addressed in the OUTPUT (stretched) domain, so
                // startOffset scales with the stretch factor. Keep the SAME source
                // window: srcSpan and the source start stay fixed, the offset and
                // duration are rescaled to the new stretch. (Without rescaling the
                // offset, the clip would start at a different point in the source.)
                double s0 = clipStretch0_ > 0 ? clipStretch0_ : 1.0;
                double srcSpan = (double) lastClickDuration_ / s0;
                if( srcSpan < 1 ) srcSpan = 1;
                newStretch = (double) newDur / srcSpan;
                newOffset = (offset_t)( (double) clipResizeOffset0_ * newStretch / s0 + 0.5 );
            }
            bool changed = newStart != clipDragStart0_ || newOffset != clipResizeOffset0_
                        || newDur != lastClickDuration_ || newLoop != clipLoopLen0_
                        || newStretch != clipStretch0_;
            if( changed ) {
                // Apply queued window parameter events first (before reverting)
                // This safely handles any invalidateCapture calls without lock contention
                cut->processWindowParamEvents();

                // Then revert to pre-drag state and re-apply via action
                lastClickSLink_->setStartTime( clipDragStart0_ );
                cut->setWindow( clipResizeOffset0_, lastClickDuration_,
                                clipLoopLen0_, clipStretch0_ );
                QList<int> clipPath = strackpath::pathOf( smv_.getModel(), lastClickTrack_ );
                clipPath.append( lastClickTrack_->indexOfChild( lastClickSLink_ ) );
                SApplication::app().submitAction(
                    new SResizeClipAction( clipPath, newStart, newOffset, newDur,
                                           newLoop, newStretch ) );
                update();
            }
        }
        clipDragArmed_ = false;
        clipDragIsSlip_ = clipDragIsStretch_ = clipDragIsLoop_ = false;
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

    // Set the cursor only if this was a pure click (no drag of any kind).
    // This allows click to seek the playhead, but dragging clips or ranges
    // won't accidentally move the playhead during the drag.
    if( rangeDrag_ == RangeNone ) {
        // Check that the mouse didn't move significantly (within 4 pixels).
        // This distinguishes a click from a small drag.
        const int CLICK_THRESHOLD = 4;
        QPoint delta = ev->pos() - lastClickPos_;
        if( delta.manhattanLength() <= CLICK_THRESHOLD ) {
            // No range drag and minimal mouse movement = pure click.
            offset_t ofs = smv_.alignTime( getTimeOf( ev->pos().x() ) );
            SApplication::app().setGlobalLocatorPos( ofs );
            if( SApplication::app().isPlaying() ) {
                smv_.model_->seekTo( SApplication::app().getGlobalLocatorPos() );
            }
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
QRect SMVActualView::getSLinkVisibRect( int trackIdx, const SLink &lk )
{
    QRect r( 0, SMV_TIME_RULER_HEIGHT+trackIdx*trackHeight_, rect().width(), trackHeight_ );
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

    // If the press lands on an existing end, grab it for moving; otherwise
    // start a brand-new range (this press fixes one end).
    if( rangeValid_ ) {
        int xs = getXPosOfOffset( rangeStart_ );
        int xe = getXPosOfOffset( rangeEnd_ );
        if( qAbs( x - xs ) <= SMV_RANGE_GRAB_PIXEL ) { rangeDrag_ = RangeMoveStart; return; }
        if( qAbs( x - xe ) <= SMV_RANGE_GRAB_PIXEL ) { rangeDrag_ = RangeMoveEnd;   return; }
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
        smv_.model_->getProject().setBPMTempo( newTempo );
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
void SMVActualView::updateHoverCursor( const QPoint &pos )
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
        int y = pos.y() - SMV_TIME_RULER_HEIGHT;
        int rowIdx = ( y + upperLeftY_ ) / trackHeight_;
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
                upper = ( ( ( y + upperLeftY_ ) % trackHeight_ ) < trackHeight_/2 );
            }
            bool onBorder = onLeft || onRight;
            Qt::KeyboardModifiers mods = QGuiApplication::keyboardModifiers();
            bool ctrl = mods & Qt::ControlModifier;
            bool alt  = mods & Qt::AltModifier;
            if( onBorder && ctrl )     { shape = Qt::SplitHCursor;     mode = "Time-stretch"; }
            else if( onRight && upper ){ custom = s_loopCursor;        mode = "Loop"; }
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

    // No button held: this is a hover — just update the gesture cursor.
    if( !( ev->buttons() & Qt::LeftButton ) ) {
        updateHoverCursor( ev->pos() );
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
    int newTrackIdx = (ev->pos().y()+upperLeftY_-SMV_TIME_RULER_HEIGHT)/trackHeight_;
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
            if( clipDragIsSlip_ ) {
                // Alt-drag the BODY: slide the content under the clip (change the
                // cut's start offset). Position and length stay put; dragging
                // right reveals earlier content.
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
                length_t contentLen = cut->getContent().hasDuration()
                                      ? (length_t) cut->getContent().getDuration() : -1;
                double st = cut->getStretch(); if( st <= 0 ) st = 1.0;
                length_t d = (length_t) smv_.alignTime( getTimeOf( ev->pos().x() ) )
                           - (length_t) smv_.alignTime( (offset_t) getLastClickOffset() );
                length_t newOff = (length_t) clipResizeOffset0_ - d;
                if( newOff < 0 ) newOff = 0;
                // Bound the window START (output domain) to near the content end,
                // not content_end - window_len: a full-length clip (window ==
                // content) would otherwise have zero slip room. Sliding further
                // simply lets the tail run into silence, which is a valid slip.
                if( contentLen >= 0 ) {
                    length_t maxOff = (length_t)( (double) contentLen * st )
                                    - (length_t) SMV_CUT_MIN_TIME;
                    if( maxOff < 0 ) maxOff = 0;
                    if( newOff > maxOff ) newOff = maxOff;
                }
                QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                // Only rebuild if offset actually changed
                if( newOff != cut->getStartOffset() ) {
                    cut->setStartOffset( (offset_t) newOff );  // Visual feedback + queues event via invalidateCapture
                    cut->ensureCapturePeaks();  // Rebuild peaks for consistent rendering
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
                double s0 = clipStretch0_ > 0 ? clipStretch0_ : 1.0;
                double srcSpan = (double) lastClickDuration_ / s0;
                if( srcSpan < 1 ) srcSpan = 1;
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
                double newStretch = (double) newDur / srcSpan;
                // Rescale the output-domain start offset by the SAME factor as the
                // stretch so the source start (startOffset/stretch) is invariant; the
                // source span is fixed by srcSpan, so the source end stays put too.
                offset_t newOffset =
                    (offset_t)( (double) clipResizeOffset0_ * newStretch / s0 + 0.5 );
                cut->setStretchRaw( newStretch );
                cut->setStartOffsetRaw( newOffset );
                cut->setDurationRaw( newDur );
                cut->ensureCapturePeaks();  // refresh container-backed previews
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
                if( newDur != cut->getDuration() || clipLoopSeg_ != cut->getLoopLength() ) {
                    cut->setLoopLengthRaw( clipLoopSeg_ );
                    cut->setDuration( newDur );
                    cut->queueWindowParamEvent( LOOP_LENGTH_CHANGE, (double) clipLoopSeg_ );
                    cut->queueWindowParamEvent( DURATION_CHANGE, (double) newDur );
                    cut->ensureCapturePeaks();  // Rebuild peaks for consistent rendering
                    smv_.getModel()->getProject().notifyArrangementChanged();  // Cascade to live assets
                }
                update( oldRect );
                update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
            } else if( lastClickedStart_ ) {
                // Drag the LEFT edge: move the clip start to the snapped mouse
                // time, trimming the front (cut start offset shifts with it).
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
                length_t maxLength = cut->getContent().hasDuration()
                                     ? (length_t) cut->getContent().getDuration() : -1;
                offset_t end0 = clipDragStart0_ + (offset_t) lastClickDuration_;  // fixed right edge
                offset_t rStart = smv_.alignTime( getTimeOf( ev->pos().x() ) );
                // Keep at least the minimum length.
                if( (length_t) end0 - (length_t) rStart < SMV_CUT_MIN_TIME )
                    rStart = end0 - (offset_t) SMV_CUT_MIN_TIME;
                // The cut's start offset can't go below 0.
                length_t shift = (length_t) rStart - (length_t) clipDragStart0_;
                if( (length_t) clipResizeOffset0_ + shift < 0 )
                    rStart = clipDragStart0_ - (offset_t) clipResizeOffset0_;
                if( (offset_t) rStart > end0 ) rStart = clipDragStart0_;   // safety
                shift = (length_t) rStart - (length_t) clipDragStart0_;
                offset_t rCutStart = (offset_t)( (length_t) clipResizeOffset0_ + shift );
                length_t rDur = (length_t) end0 - (length_t) rStart;
                if( maxLength >= 0 && rDur > maxLength - (length_t) rCutStart )
                    rDur = maxLength - (length_t) rCutStart;
                if( rDur >= SMV_CUT_MIN_TIME ) {
                    QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                    cut->setStartOffset( rCutStart );
                    cut->setDuration( rDur );
                    lastClickSLink_->setStartTime( rStart );
                    cut->invalidateCapture();  // Drop cached render
                    // Force synchronous rebuild for live feedback during drag
                    cut->ensureCapture();
                    cut->ensureCapturePeaks();
                    smv_.getModel()->getProject().notifyArrangementChanged();  // Cascade to live assets
                    update( oldRect );
                    update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
                }
            } else if( lastClickedEnd_ ) {
                // Drag the RIGHT edge: set the duration to the snapped mouse time.
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
                length_t maxLength = cut->getContent().hasDuration()
                                     ? (length_t) cut->getContent().getDuration() : -1;
                offset_t rEnd = smv_.alignTime( getTimeOf( ev->pos().x() ) );
                length_t rDur = (length_t) rEnd - (length_t) clipDragStart0_;
                if( rDur < SMV_CUT_MIN_TIME ) rDur = SMV_CUT_MIN_TIME;
                if( maxLength >= 0 ) {
                    length_t maxDur = maxLength - (length_t) clipResizeOffset0_;
                    if( rDur > maxDur ) rDur = maxDur;
                }
                QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                // Only rebuild if duration actually changed (not clamped to same value)
                if( rDur != cut->getDuration() ) {
                    cut->setDuration( rDur );
                    cut->queueWindowParamEvent( DURATION_CHANGE, (double) rDur );
                    cut->ensureCapturePeaks();  // Rebuild peaks for consistent rendering
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

    if( ev->buttons() & Qt::RightButton ) {
	// Also emulate legacy right mouse button events for the events
	// we do not receive via QContextHelpEvent
        qGlobalPopup_->popup( mapToGlobal( ev->pos() ) );
    } else if( ev->buttons() & Qt::LeftButton ) {
        // Detect, on which object we clicked.
        // We know the track,  so now calculate the time.
        if( lastClickTrack_ ) {
            if( lastClickSLink_ ) {
                Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
                bool onBorder = lastClickedStart_ || lastClickedEnd_;
                if( (modifiers & Qt::ControlModifier) && !onBorder ) {
                    // Ctrl-click on a clip BODY: duplicate it and drag the live copy.
                    // (Ctrl on a border means time-stretch — handled below.)
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
                    // release). Which gesture: Alt on the body = slip; Ctrl on a
                    // border = time-stretch; right-edge upper half = loop; plain
                    // border = resize; plain body = move. Snapshot the full cut
                    // window so the release can revert-then-action.
                    bool alt = modifiers & Qt::AltModifier;
                    clipDragArmed_ = true;
                    clipDragIsDuplicate_ = false;
                    clipDragIsSlip_    = ( alt && !onBorder );
                    clipDragIsStretch_ = ( (modifiers & Qt::ControlModifier) && onBorder );
                    clipDragIsLoop_    = ( !(modifiers & Qt::ControlModifier)
                                           && lastClickedEnd_ && lastClickedEndUpper_ );
                    clipLoopSeg_ = 0;   // captured lazily on the first loop move
                    clipDragTrack0_ = lastClickTrack_;
                    clipDragStart0_ = lastClickSLink_->getStartTime();
                    {
                        SObject &o = lastClickSLink_->getSObject();
                        if( qstrcmp( o.metaObject()->className(), "SCut" ) == 0 ) {
                            SCut *c = (SCut*)&o;
                            clipResizeOffset0_ = c->getStartOffset();
                            clipLoopLen0_      = c->getLoopLength();
                            clipStretch0_      = c->getStretch();
                        } else {
                            clipResizeOffset0_ = 0;
                            clipLoopLen0_      = 0;
                            clipStretch0_      = 1.0;
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

void SStdMixerView::appendRowsFor( SObject *container, int depth )
{
    for( SLink *lk : container->childLinks() ) {
        STrack *tk = dynamic_cast<STrack*>( &lk->getSObject() );
        if( !tk ) continue;          // clips render inside their track's own lane
        bool kids = hasChildTracks( tk );
        bool col = collapsed_.contains( tk );
        rows_.append( STrackRow{ tk, lk, container, depth, kids, col } );
        if( kids && !col ) appendRowsFor( tk, depth+1 );   // recurse if expanded
    }
}

void SStdMixerView::rebuildRows()
{
    rows_.clear();
    if( model_ ) appendRowsFor( model_, 0 );
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
    int h = getTrackHeight();
    for( int i=0; i<rows_.size(); ++i ) {
        const STrackRow &row = rows_.at( i );
        SSMVMixerControl *mc = new SSMVMixerControl( qTrackControlBox_, *this, *row.track );
        mc->setTreeInfo( row.depth, row.hasChildren, row.collapsed );
        mc->move( 0, h*i );
        mc->show();
        controlArray_->append( mc );
    }
    qTrackControlBox_->resize( SMV_TRACK_CTRL_WIDTH, h*rows_.size() );
}

// Single entry point for any structural change (add/remove/reorder/group/fold):
// rebuild the flattened tree, the control column, the scroll range, and repaint.
void SStdMixerView::refreshTrackTree()
{
    rebuildRows();
    rebuildControlColumn();
    nTracksChanged();
    qContent_->update();
}

// The model signals still arrive incrementally; a full refresh is simplest and
// correct (nesting changes do not map cleanly onto add/remove-at-index).
void SStdMixerView::addMixerControl( int, STrack & )    { refreshTrackTree(); }
void SStdMixerView::removeMixerControl( int, STrack & ) { refreshTrackTree(); }
void SStdMixerView::tracksReordered()                   { refreshTrackTree(); }

// Map a Y in the control-column to an insertion gap 0..n among the visible lanes.
int SStdMixerView::insertSlotAt( int y ) const
{
    int h = getTrackHeight();
    if( h<=0 ) return 0;
    int n = rowCount();
    int slot = (y + h/2) / h;
    if( slot<0 ) slot = 0;
    if( slot>n ) slot = n;
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
    int h = getTrackHeight();
    int n = rowCount();
    if( h>0 ) {
        int r = y / h;
        if( r>=0 && r<n ) {
            int within = y - r*h;
            const STrackRow *row = rowAt( r );
            // Over the middle half of a lane -> nest onto that track.
            if( row && within > h/4 && within < (3*h)/4 ) *onto = row->track;
        }
    }
    // Insertion gap among top-level lanes = how many sit above the drop.
    int slot = 0;
    for( int i=0; i<n; ++i ) {
        if( rowAt( i )->depth != 0 ) continue;
        if( y > i*h + h/2 ) ++slot;
    }
    *topSlot = slot;
}

void SStdMixerView::updateTrackDrag( int yInControlBox )
{
    if( !dragControl_ || !dropIndicator_ ) return;
    STrack *onto = NULL; int slot = 0;
    resolveDrop( yInControlBox, &onto, &slot );
    int h = getTrackHeight();
    if( onto && onto != &dragControl_->getTrack() ) {
        // Nest: outline the whole target lane.
        int r = rowIndexOfTrack( onto );
        dropIndicator_->setStyleSheet( "border:2px solid #2080ff; background:transparent;" );
        dropIndicator_->setGeometry( 0, r*h, SMV_TRACK_CTRL_WIDTH, h );
    } else {
        // Between: a thin insertion line at the nearest lane boundary.
        dropIndicator_->setStyleSheet( "background:#2080ff; border:none;" );
        int yLine = insertSlotAt( yInControlBox )*h;
        if( yLine>0 ) yLine -= 1;
        dropIndicator_->setGeometry( 0, yLine, SMV_TRACK_CTRL_WIDTH, 3 );
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
    STrack *t = &control->getTrack();
    int ri = rowIndexOfTrack( t );
    SObject *curParent = (ri>=0) ? rowAt( ri )->parent : NULL;

    STrack *onto = NULL; int slot = 0;
    resolveDrop( yInControlBox, &onto, &slot );

    // Dropped onto a lane:
    if( onto ) {
        // Onto itself or the parent it already sits in -> no-op (don't submit a
        // doomed reparent).
        if( onto == t || (SObject*)onto == curParent ) return;
        // Otherwise nest under it (the action also guards cycles).
        SApplication::app().submitAction( new SReparentTrackAction(
            strackpath::pathOf( model_, t ),
            strackpath::pathOf( model_, onto ), -1 ) );
        return;
    }

    // Dropped on a boundary -> reorder at top level, or pop a nested track out.
    int nTop = model_->getNTracks();
    int fromTop = model_->indexOfChildObject( *t );
    if( fromTop>=0 ) {
        int target = (slot>fromTop) ? slot-1 : slot;
        if( target<0 ) target = 0;
        if( target>=nTop ) target = nTop-1;
        if( target==fromTop ) return;
        SApplication::app().submitAction( new SMoveTrackAction( QList<int>{ fromTop }, target ) );
    } else {
        int target = slot;
        if( target<0 ) target = 0;
        if( target>nTop ) target = nTop;
        SApplication::app().submitAction( new SReparentTrackAction(
            strackpath::pathOf( model_, t ), QList<int>{}, target ) );
    }
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
	qWarning( "SStdMixerView::timeSliderMoved(): newValue was less than zero." );
	newValue = 0;
    }
    qWarning( "SStdMixerView::timeSliderMoved(): newValue=%d.",
	      newValue );
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
 * Please note, the link pointer passed (though not invalid),
 * is used as link inside the new cut.
 */
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
    SCut *sc = new SCut( (SProject *)(so->parent()), *lk ); 
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
    int h = qContent_->height();
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
    h /= qContent_->getTrackHeight();
    qScrollVert_->setPageStep( h );
    qScrollVert_->setMaximum( rowCount()-h );
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

void SStdMixerView::zoomInVert()
{
    // FIXME: Range checking.
    int h = qContent_->getTrackHeight();
    // FIXME: Configure this
    h = (h*3)/2;
    qContent_->setTrackHeight( h );
    qTrackControlBox_->resize( SMV_TRACK_CTRL_WIDTH, getTrackHeight()*rowCount() );
}

void SStdMixerView::zoomOutVert()
{
    // FIXME: Range checking.
    int h = qContent_->getTrackHeight();
    // FIXME: Configure this
    h = (h*2)/3;
    qContent_->setTrackHeight( h );
    qTrackControlBox_->resize( SMV_TRACK_CTRL_WIDTH, getTrackHeight()*rowCount() );
}

void SStdMixerView::setBPMTempo( double bpmTempo )
{
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

offset_t SSnapSpec::alignTime( offset_t o )
{
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
    // setBackgroundMode( NoBackground );
    qGlobalPopup_ = new QMenu( this );
    QObject::connect( qGlobalPopup_, SIGNAL( aboutToShow() ),
                      this, SLOT( ctGlobalShow() ) );

    // Context menu for the time-range bar (top ruler).
    qRangePopup_ = new QMenu( this );
    qRangePopup_->addAction( "Set &BPM...", this, SLOT( ctRangeSetBPM() ) );
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

//    setBackgroundColor( QColor( 0, 0, 0 ) );
    trackHeight_ = 100;
    secondWidth_ = 30.;
    upperLeftX_ = upperLeftY_ = 0;

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
}

int SMVActualView::wheelActionFor( Qt::KeyboardModifiers mods ) const
{
    bool ctrl  = mods & Qt::ControlModifier;
    bool shift = mods & Qt::ShiftModifier;
    if( ctrl && shift ) return wheelCtrlShift_;
    if( ctrl )          return wheelCtrl_;
    if( shift )         return wheelShift_;
    return wheelPlain_;
}

void SMVActualView::wheelEvent( QWheelEvent *ev )
{
    int dy = ev->angleDelta().y();
    if( dy == 0 ) { QWidget::wheelEvent( ev ); return; }
    int dir = (dy > 0) ? +1 : -1;   // +1 = wheel away from the user ("up")

    switch( wheelActionFor( ev->modifiers() ) ) {

    case SOpt::ScrollVertical: {
        // One track lane per notch via the scrollbar (keeps it in sync).
        if( smv_.qScrollVert_ ) {
            smv_.qScrollVert_->setValue( smv_.qScrollVert_->value() - dir );
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
        if( wheelZoomToCursor_ ) {
            int mouseX = (int) ev->position().x();
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
        setTrackHeight( h );
        smv_.qTrackControlBox_->resize( SMV_TRACK_CTRL_WIDTH,
                                        getTrackHeight() * smv_.rowCount() );
        break;
    }

    case SOpt::None:
    default:
        QWidget::wheelEvent( ev );
        return;
    }
    ev->accept();
}

void SMVActualView::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasFormat(QStringLiteral("application/x-smaragd-resource"))) {
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
    if (!mimeData->hasFormat(QStringLiteral("application/x-smaragd-resource"))) {
        return;
    }

    QString payload = QString::fromUtf8(mimeData->data(QStringLiteral("application/x-smaragd-resource")));
    if (payload.isEmpty()) {
        return;
    }

    e->acceptProposedAction();

    // Compute drop position (time + track).
    offset_t timePos = smv_.alignTime(getTimeOf((int)e->position().x()));
    int rowIdx = (int)((e->position().y() + upperLeftY_ - SMV_TIME_RULER_HEIGHT) / trackHeight_);

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
    SObject *root = project->getRootComponent();
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
    if (!mixer) {
        return;
    }

    using namespace strackpath;
    QList<int> trackPath = pathOf(root, track);

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
        // The file's track index is its position in the mixer's top-level children.
        int trackIdx = mixer->indexOfChildObject(*track);
        if (trackIdx >= 0) {
            SApplication::app().submitAction(new SAddSampleAction(trackIdx, filePath, timePos));
        }
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
    controlArray_ = new QVector<SSMVMixerControl*>();

    qGridLayout_ = new QGridLayout( this /* , 4, 5 */ );    
    qContent_ = new SMVActualView( this, *this );
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
    qTrackControlBoxHolder_->setFixedWidth( SMV_TRACK_CTRL_WIDTH );
    // Double-clicking the blank area below the track heads adds a new track.
    qTrackControlBoxHolder_->installEventFilter( this );
    // qTrackControlBoxHolder_->setBackgroundColor( QColor( 100, 100, 0 ) );
    qGridLayout_->addWidget(
        qTrackControlBoxHolder_,
        0, /* fromRow */ 
        0, /* fromCol */
        4, /* rowSpan */
        1  /* colSpan */ 
        );
    // was: qGridLayout_->addMultiCellWidget( qTrackControlBoxHolder_, 0 /* from Row */, 3 /* to Row */, 0 /* fromCol */, 0 /* toCol */ );
    

    qTrackControlBox_ = new QWidget( qTrackControlBoxHolder_ );

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
    }

    QObject::connect( model_, SIGNAL( durationChanged( length_t ) ), 
                      this, SLOT( contentDurationChanged( length_t ) ) );
    QObject::connect( &(SApplication::app()), SIGNAL( globalLocatorMoved( offset_t, offset_t ) ), 
                      qContent_, SLOT( globalLocatorMoved( offset_t, offset_t ) ) );
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
                      this, SLOT( setBPMTempo( double ) ) );

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

    // Build the flattened tree + control column for whatever already resides in
    // the mixer (refreshTrackTree handles rows, controls and scroll range).
    {
        refreshTrackTree();
        contentDurationChanged( model_->getDuration() );
        update();
    }

    // Load saved track control width
    loadTrackControlWidth();

    // Create the track detail panel (bottom of mixer view)
    qTrackDetailPanel_ = new STrackDetailPanel(this);
    qGridLayout_->addWidget(qTrackDetailPanel_, 4, 0, 1, 5);
    qGridLayout_->setRowStretch(4, 0);  // Don't stretch detail panel

    // Connect mixer's track selection to detail panel
    connect(model_, &SStdMixer::selectedTrackChanged,
            qTrackDetailPanel_, &STrackDetailPanel::setTrack);
}

void SStdMixerView::setTrackControlWidth( int width )
{
    // Clamp between minimal and standard
    if( width < TRACK_CTRL_WIDTH_MINIMAL ) width = TRACK_CTRL_WIDTH_MINIMAL;
    if( width > TRACK_CTRL_WIDTH_STANDARD ) width = TRACK_CTRL_WIDTH_STANDARD;

    if( trackControlWidth_ != width ) {
        trackControlWidth_ = width;
        qTrackControlBoxHolder_->setFixedWidth( width );
        saveTrackControlWidth();
        update();
    }
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
        event->x() >= dividerX - 5 && event->x() <= dividerX + 5 ) {
        trackHeaderDragActive_ = true;
        trackHeaderDragStartX_ = event->x();
        trackHeaderDragStartWidth_ = trackControlWidth_;
        event->accept();
        return;
    }
    QWidget::mousePressEvent( event );
}

void SStdMixerView::mouseMoveEvent( QMouseEvent *event )
{
    if( trackHeaderDragActive_ ) {
        int delta = event->x() - trackHeaderDragStartX_;
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
