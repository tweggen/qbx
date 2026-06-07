
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

#include "twwavinput.h"
#include "twspeaker.h"
#include "sapplication.h"
#include "sstdmixer.h"
#include "sstdmixerview.h"
#include "strack.h"
#include "sobjectrenderer.h"
#include "splainwave.h"
#include "slink.h"
#include "scut.h"
#include "sproject.h"
#include "sprojectprops.h"
#include "ssettings.h"
#include "ssmvmixercontrol.h"
#include "actions/saddtrackaction.h"
#include "actions/smovetrackaction.h"
#include "actions/sreparenttrackaction.h"
#include "actions/sremovetrackaction.h"
#include "actions/smoveclipaction.h"
#include "actions/ssplitclipaction.h"
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
//    update();
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
    int newUpperLeftX = ((int)((((double)leftOffset)/44100.)*secondWidth_));
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
    // FIXME: 44100.
    return ((int)((((double)off)/44100.)*secondWidth_))-upperLeftX_;
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
    SLink *oldLink = qContent_->getLastClickSLink();
    if( !oldLink ) {
        qWarning( "ctRemoveSample called without object.\n" );
        return;
    }
    QRect r = qContent_->getSLinkVisibRect( qContent_->getLastClickTrackIdx(), *oldLink );
    qContent_->resetLastClickSLink();
    delete oldLink;
    // FIXME: Only the track.
    qContent_->update( r );
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
        qGlobalPopup_->addAction( "&Split object", &smv_, SLOT( ctSplitSample() ) );
        qGlobalPopup_->addAction( "Add &link", &smv_, SLOT( ctAddLink() ) );
        qGlobalPopup_->addSeparator();
    }
    if( lastClickTrack_ ) {
        qGlobalPopup_->addAction( smv_.actInsertSample_ );
        qGlobalPopup_->addAction( "&Remove sample", &smv_, SLOT( ctRemoveSample() ) );
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
    // FIXME: Remove the 44100.
    offset_t totalX = x + getUpperLeftX();
    double a = ((double)totalX)/getSecondWidth()*44100.;
    return (offset_t)a;
}

void SMVActualView::updateLastClickVars( const QPoint &pos )
{
    lastClickedStart_ = lastClickedEnd_ = false;
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
                    }                    
                }
            }
        }
    } else {
        lastClickTrack_ = NULL;
        lastClickSLink_ = NULL;
    }    
}

void SMVActualView::mouseReleaseEvent( QMouseEvent *ev )
{
    if( rangeDrag_ != RangeNone ) {
        endRangeDrag( ev->pos().x() );
        return;
    }

    // Finalize a clip MOVE as a single undoable action. The drag mutated the
    // model live for feedback; here we revert to the pre-drag placement and
    // re-apply it through SMoveClipAction so it lands as one undo step. (Resize
    // — lastClickedStart_/End_ — is not yet actioned.)
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
    double startTime = ((double)startTimeOfs)/44100.;
    int startPos = (int)(startTime*secondWidth_);
    startPos -= upperLeftX_;
    if( startPos>=0 && startPos<width() ) r.setLeft( startPos );
    if( lk.getSObject().hasDuration() ) {
        startTimeOfs += lk.getSObject().getDuration();
        startTime = ((double)startTimeOfs)/44100.;
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
    update();
}

void SMVActualView::drawRange( QPainter &p, const QRect &myRect )
{
    if( !rangeValid_ ) return;
    offset_t lo, hi; rangeBounds( lo, hi );
    int xlo = getXPosOfOffset( lo );
    int xhi = getXPosOfOffset( hi );

    // Grey band inside the ruler.
    if( xhi > xlo ) {
        p.fillRect( QRect( xlo, 0, xhi - xlo, SMV_TIME_RULER_HEIGHT ),
                    QColor( 150, 150, 150 ) );
    }
    // Edges as vertical lines over all tracks.
    p.setPen( QColor( 80, 80, 80 ) );
    if( xlo >= 0 && xlo < myRect.width() ) p.drawLine( xlo, 0, xlo, myRect.height()-1 );
    if( xhi >= 0 && xhi < myRect.width() ) p.drawLine( xhi, 0, xhi, myRect.height()-1 );
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
    update();
}

void SMVActualView::ctRangeCreateAsset()
{
    // TODO: feature (b) — turn the selected region into a reusable live asset
    // (see plan/proposed/05_TRACK_GROUPING_AND_LIVE_ASSETS.md). Stub for now.
    if( !rangeValid_ ) return;
    qWarning( "Create asset from range [%lld..%lld]: not yet implemented.",
              (long long) getRangeStart(), (long long) getRangeEnd() );
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

    // Check scrolling, if the event position is invisible.
    QRect myRect = rect();
    if( ev->pos().x()<0 ) {
        int currentOffset = upperLeftX_;
        int d = -ev->pos().x();
        currentOffset -= d;
        if( currentOffset<0 ) currentOffset = 0;        
        if( currentOffset != upperLeftX_ ) {
            setLeftOffset( (offset_t)( ((double)currentOffset)/secondWidth_*44100.) );
        }
    } else if( ev->pos().x()>=myRect.width() ) {
        int currentOffset = upperLeftX_;
        int d = ev->pos().x()-myRect.width();
        currentOffset += d;
        if( currentOffset != upperLeftX_ ) {
            setLeftOffset( (offset_t)( ((double)currentOffset)/secondWidth_*44100.) );
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

            if( lastClickedStart_ && delta != 0 ) {
                // Drag the start?
                // First ensure, this is an scut link.
                // (After the first move, it will remain being an scut)
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
                length_t maxLength = -1;
                if( cut->getContent().hasDuration() ) {
                    maxLength = cut->getContent().getDuration();
                }
                length_t newDuration = lastClickDuration_-delta;
                if( maxLength>=0 && newDuration > maxLength ) newDuration = maxLength;
                offset_t oldCutStart = cut->getStartOffset();
                // Calculate, how much we already dragged.
                offset_t prevStart = lastClickSLink_->getStartTime();
                length_t prevDelta = prevStart-getLastClickStartOffset();
                // Now, calculate, how much we changed this time
                length_t thisDelta = delta-prevDelta;
                length_t oldStart = getLastClickStartOffset();
                if( ((delta<0 && ((offset_t)(-delta))<(offset_t)oldStart) || delta>0)
                    && ((thisDelta<0 && ((offset_t)(-thisDelta))<oldCutStart) || thisDelta>0)
                    && newDuration>SMV_CUT_MIN_TIME ) {
                    QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                    offset_t newStart = oldStart + delta;
                    offset_t newCutStart = oldCutStart + thisDelta;
                    // This only makes sense, if the cut does not vanish.
                    cut->setDuration( newDuration );
                    cut->setStartOffset( newCutStart );
                    lastClickSLink_->setStartTime( newStart );
                    update( oldRect );
                    update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
                }
            } else if( lastClickedEnd_ && delta != 0) {
                // Drag the end?
                // First ensure, this is an scut link.
                // (After the first move, it will remain being an scut)
                lastClickSLink_ = smv_.ensureSCut( lastClickSLink_ );
                SCut *cut = (SCut *)&(lastClickSLink_->getSObject());
                length_t maxLength = -1;
                if( cut->getContent().hasDuration() ) {
                    maxLength = cut->getContent().getDuration();
                }
                length_t newDuration = lastClickDuration_+delta;
                if( maxLength>=0 && newDuration > maxLength ) newDuration = maxLength;
                if( newDuration>SMV_CUT_MIN_TIME ) {
                    QRect oldRect = getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ );
                    // This only makes sense, if the cut does not vanish.
                    cut->setDuration( newDuration );
                    update( oldRect );
                    update( getSLinkVisibRect( lastClickTrackIdx_, *lastClickSLink_ ) );
                }
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
            }
        } else {
            // OK, seek to the position.
            offset_t ofs = getTimeOf( ev->pos().x() );            
            ofs = smv_.alignTime( ofs );
            SApplication::app().setGlobalLocatorPos( ofs );
            // FIXME: Application should do that!
            if( SApplication::app().isPlaying() ) {
                smv_.model_->seekTo( SApplication::app().getGlobalLocatorPos() );
            }
        }
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
        bool gotObject = false;
        if( lastClickTrack_ ) {            
            if( lastClickSLink_ ) {
                lastClickSelStartOffset_ = lastClickSLink_->getStartTime();
                // Snapshot for a possible MOVE drag (finalized in release).
                clipDragArmed_ = true;
                clipDragTrack0_ = lastClickTrack_;
                clipDragStart0_ = lastClickSLink_->getStartTime();
                // OK, we clicked on an slink, process the selection stuff.
                // Qt::MouseButtons buttons = ev->buttons();
                Qt::KeyboardModifiers modifiers = QGuiApplication::keyboardModifiers();
                switch( modifiers & (Qt::ShiftModifier|Qt::ControlModifier) ) {
                case 0: // No modifier, new one becomes selected.
                    SApplication::app().setSelectedSLink( lastClickSLink_ );
                    break;
                case Qt::ShiftModifier: // Shuft Button: Add this object to the selection.
                    if( SApplication::app().isSLinkSelected( lastClickSLink_ ) ) {
                        SApplication::app().unselectSLink( lastClickSLink_ );
                    } else {
                        SApplication::app().addSelectedSLink( lastClickSLink_ );
                    }
                    break;
                }
                // FIXME: Only update the object itselves.
                update();
                gotObject = true;
            }
        }
        if( !gotObject ) {
            // OK, seek to the position.
            offset_t ofs = getTimeOf( ev->pos().x() );
            ofs = smv_.alignTime( ofs );
            SApplication::app().setGlobalLocatorPos( ofs );
            if( SApplication::app().isPlaying() ) {
                // FIXME: Why here? It should work anyways.
                //SApplication::app().getSpeaker()->stopOutput();
                smv_.model_->seekTo( SApplication::app().getGlobalLocatorPos() );
                //SApplication::app().getSpeaker()->startOutput();
            }
        }
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
    qWarning( "Setting maxValue to %d.\n", (int) newDur );
    qScrollHoriz_->setMaxValue( (int)newDur-pageStep );
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
    qScrollVert_->setMaximum( (int) newNTracks-pageStep );
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
    double dw = w;
    dw = dw * 44100. / qContent_->getSecondWidth();
    offset_t lw = (offset_t) (dw);
    if( lw>0x7fffffff ) lw = 0x7fffffff;
    offset_t dur = 1;
    if( model_->hasDuration() ) {
	dur = model_->getDuration();
    }
    // FIXME: Remove the 44100
    dw = HSliderRange*(double)w/qContent_->getSecondWidth()*44100. / (double)dur;
    qScrollHoriz_->setPageStep( (int)(dw+0.5) );
    qScrollHoriz_->setSingleStep( ((int)(dw+0.5)/10)+1 );
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
    double dw = w;
    dw = dw * 44100. / qContent_->getSecondWidth();
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
        // FIXME: 44100
        w *= 44100.;
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
    qRangePopup_->addAction( "Create &asset from range", this, SLOT( ctRangeCreateAsset() ) );

    rangeValid_ = false;
    rangeStart_ = rangeEnd_ = 0;
    rangeDrag_ = RangeNone;

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

    // Build the flattened tree + control column for whatever already resides in
    // the mixer (refreshTrackTree handles rows, controls and scroll range).
    {
        refreshTrackTree();
        contentDurationChanged( model_->getDuration() );
        update();
    }
}

SStdMixerView::~SStdMixerView()
{
    delete currentSnapSpec_;
    delete controlArray_;
}
