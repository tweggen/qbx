
#include <stdlib.h>
#include <cmath>

#include <qwidget.h>
#include <qevent.h>
#include <qpainter.h>
#include <qmenu.h>
#include <qlayout.h>
#include <qmessagebox.h>
#include <qinputdialog.h>
#include <qslider.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qpushbutton.h>
#include <QApplication>
#include <QPolygon>
#include <QSignalBlocker>
#include <QCursor>
#include <QToolTip>

#include "app/shell/sapplication.h"
#include "app/shell/smainwindow.h"
#include "app/shell/slivemonitor.h"
#include "app/shell/slivemonitor.h"
#include "app/shell/smidiinputhub.h"
#include "tw/devices/audio_input.h"
#include "app/shell/ssettings.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/timeline/sstdmixerview.h"
#include "app/timeline/ssubmit.h"
#include "app/timeline/slevelmeter.h"
#include "app/timeline/sfadercurve.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/sliveinputactions.h"
#include "app/objects/track/sliveinputactions.h"
#include "app/model/slink.h"
#include "app/model/sobjectrenderer.h"
#include "app/model/sproject.h"
#include "app/model/ssolorules.h"
#include "app/timeline/ssmvmixercontrol.h"
#include "app/objects/track/strackcolormodifier.h"
#include "app/objects/track/ssettrackvolumeaction.h"
#include "app/objects/track/seteditgroupaction.h"
#include "app/objects/track/ssettrackmuteaction.h"
#include "app/objects/track/ssettracknameaction.h"
#include "app/objects/track/ssettracksoloaction.h"
#include "app/model/seditgroups.h"
#include "app/model/sobjectpath.h"
#include "app/model/splacements.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/pluginui/spluginparamereditor.h"
#include "app/model/sobjectpath.h"
#include "app/actions/sactionhistory.h"
#include "app/objects/track/sautomationactions.h"
#include "app/shell/sautomationrecorder.h"
#include <QUndoStack>
#include <QPair>


// The fader mapping now lives in app/timeline/sfadercurve.h, because the Track
// Detail dock drives the same track volume and has to agree with this one.
// Local aliases keep the call sites below unchanged.
static inline double sliderToDB( int v )   { return sFaderToDb( v ); }
static inline int    dbToSlider( double d ) { return sDbToFader( d ); }
static const int FADER_MIN = SFADER_MIN;
static const int FADER_MAX = SFADER_MAX;

// Width of the grip strip down the left side of the control that acts as the
// track-reorder drag handle, and the pixels the pointer must travel before a
// press there turns into a drag.
static const int HANDLE_W = 12;
static const int DRAG_THRESHOLD = 4;

QSize SSMVMixerControl::sizeHint() const
{
    // The view places this head at its lane's geometry (setGeometry), so the
    // hint only has to be non-binding: report what we already are.
    return QSize( width(), height() > 0 ? height() : smv_.getTrackHeight() );
}

/**
 * Called from QSlider
 */
void SSMVMixerControl::sliderValueChanged( int value )
{
    applyVolume_( sliderToDB( value ) );
}

/**
 * Apply a new track volume (in dB), routing through the action system so it is
 * undoable and (once the engine drain goes async) coalescable. Address our track
 * by index-PATH from the root mixer. This used to scan the mixer's DIRECT
 * children for our track, which returned -1 for a track nested in a folder, so
 * a grouped lane's fader silently took the non-undoable fallback below. Same
 * resolution soloToggled() uses; the old top-level scan helper is gone, so
 * nobody can reach for it again.
 */
void SSMVMixerControl::applyVolume_( double newVolume )
{
    SStdMixer *mixer = smv_.getModel();
    const QList<int> trackPath =
        mixer ? strackpath::pathOf( mixer, &tk_ ) : QList<int>();
    // pathOf() returns {} for "the root itself" as well as "not found", but tk_
    // is an STrack and can never BE the root, so empty means unresolvable.
    if( !trackPath.isEmpty() ) {
        // A Touch/Latch/Write pass buffers on the UI thread and commits ONE
        // set-automation-points when the gesture ends (proposal 37 P6, D5) - so
        // a fader drag during playback must NOT submit an action per tick.
        SAutomationRecorder::Target t;
        t.ownerPath = trackPath;
        t.target = QStringLiteral( "self:Volume" );
        if( SApplication::app().isPlaying()
            && SApplication::app().automationRecorder().writeTick(
                   t, newVolume,
                   smv_.contentView() ? smv_.contentView()->localLocatorPos()
                                      : SApplication::app().getGlobalLocatorPos() ) )
            return;
        stimeline::submitActive(
            new SSetTrackVolumeAction( trackPath, newVolume ) );
    } else {
        tk_.setVolume( newVolume );
        // Direct mutation doesn't go through actions, so invalidate caches manually
        // (asset captures need to reflect volume changes).
        if( SProject *p = SApplication::app().getCurrentProject() )
            p->notifyArrangementChanged();
    }
}

/**
 * Wheel over a track head = wheel over the arranger canvas. The head column is
 * part of the same view, so scrolling/zooming there had to work; without this
 * the event just died in the head (QWidget's default ignores it, and the column
 * viewport is not in the canvas' parent chain).
 *
 * The fader is the one exception: it accepts the wheel itself (1.0 dB per
 * notch, see its singleStep), so those events never reach here.
 */
void SSMVMixerControl::wheelEvent( QWheelEvent *ev )
{
    if( !smv_.wheelFromHead( ev ) )
        QWidget::wheelEvent( ev );
}

/**
 * Double-clicking the fader resets it to unity gain (0.0 dB). We intercept the
 * event here rather than subclass QSlider; the slider would otherwise treat the
 * second press as the start of another drag.
 *
 * Also intercept mouse presses on the track label to allow selecting the track
 * from anywhere on the control (d).
 *
 * And intercept Home/End on the fader (item j): QAbstractSlider's own
 * keyPressEvent maps them to "jump to minimum/maximum", so a fader that
 * happens to have keyboard focus would silently steal the transport's
 * go-to-start/-end shortcut. Home/End must always drive the transport,
 * whether or not a fader has focus, so this catches them BEFORE QSlider's
 * handler ever sees them and forwards to the same SMainWindow slots the
 * window-wide shortcuts use.
 */
bool SSMVMixerControl::eventFilter( QObject *watched, QEvent *ev )
{
    if( watched == qVolume_ && ev->type() == QEvent::KeyPress ) {
        QKeyEvent *ke = static_cast<QKeyEvent *>( ev );
        if( ke->key() == Qt::Key_Home || ke->key() == Qt::Key_End ) {
            // NOT QApplication::activeWindow(): a --test-case run never
            // shows the window (main.cpp), so nothing is ever "active" under
            // QT_QPA_PLATFORM=offscreen and that call always returns null
            // there. topLevelWidgets() is the same lookup every testkit
            // gesture verb uses (see strackselectionactions.cpp's
            // mainWindow()) for exactly this reason.
            SMainWindow *mw = nullptr;
            for( QWidget *w : QApplication::topLevelWidgets() ) {
                if( ( mw = qobject_cast<SMainWindow*>( w ) ) ) break;
            }
            if( mw ) {
                if( ke->key() == Qt::Key_Home ) mw->gotoRangeStart();
                else                            mw->gotoRangeEnd();
            }
            return true;   // never let QAbstractSlider touch the fader's value
        }
    }
    if( watched == qVolume_ && ev->type() == QEvent::MouseButtonDblClick ) {
        QMouseEvent *me = static_cast<QMouseEvent *>( ev );
        if( me->button() == Qt::LeftButton ) {
            applyVolume_( 0.0 );
            return true;   // swallow it so the slider doesn't also jump-to-position
        }
    }
    // Pass mouse press events from the track label through to parent for selection (d)
    if( watched == qTrkLabel_ && ev->type() == QEvent::MouseButtonPress ) {
        QMouseEvent *me = static_cast<QMouseEvent *>( ev );
        if( me->button() == Qt::LeftButton ) {
            // Modifiers off the EVENT, so Ctrl/Shift work here exactly as they
            // do on the strip itself.
            smv_.applyTrackSelectionClick( &tk_, me->modifiers() );
            return false;  // Let the label still handle it for editing
        }
    }
    return QWidget::eventFilter( watched, ev );
}

/**
 * Called by SObject, if track volume changes (model -> view).
 */
void SSMVMixerControl::sliderValueChanged( double value )
{
    setSliderSilently( value );
}

/**
 * Move the fader to reflect a model-side volume change WITHOUT emitting
 * valueChanged() (which would submit a redundant action and corrupt the undo
 * stack during an undo). Always refreshes the dB readout.
 */
void SSMVMixerControl::setSliderSilently( double value )
{
    int newValue = dbToSlider( value );
    if( qVolume_->value() != newValue ) {
        QSignalBlocker block( qVolume_ );
        qVolume_->setValue( newValue );
    }
    if( qVolLabel_ ) {
        qVolLabel_->setText( QString::asprintf( "%+.1f dB", value ) );
    }
}

void SSMVMixerControl::setTreeInfo( int depth, bool foldable, bool collapsed )
{
    depth_ = depth;
    foldable_ = foldable;
    collapsed_ = collapsed;
    // Indent the content: [depth indent][fold gutter][grip][content].
    int left = depth_*SMV_TRACK_INDENT + SMV_FOLD_W + HANDLE_W;
    qLayout_->setContentsMargins( left, 2, 4, 2 );
    update();
}

// The grip's left edge, accounting for indent + fold gutter.
static inline int gripLeft( int depth ) { return depth*SMV_TRACK_INDENT + SMV_FOLD_W; }

// Draw the indented grip strip and, for parents, a fold triangle to its left.
void SSMVMixerControl::paintEvent( QPaintEvent *ev )
{
    // Base colour by selection state. The PRIMARY (the lane the Track Detail
    // dock follows) is the brightest, other selected lanes sit between it and
    // an unselected one — so a multi-selection is visible as a block without
    // losing which lane is the anchor.
    QColor baseColor = primary_  ? QColor( 64, 100, 140 )
                     : selected_ ? QColor( 56, 86, 122 )
                                 : QColor( 48, 70, 100 );

    // Apply track state modifiers (muted, solo, armed for recording)
    STrackColorModifier mod = STrackColorModifier::fromTrackState( tk_ );
    QColor bgColor = mod.apply( baseColor );

    QPainter p( this );
    p.fillRect( rect(), bgColor );

    QWidget::paintEvent( ev );

    int gx = gripLeft( depth_ );
    QRect handle( gx, 0, HANDLE_W, height() );
    p.fillRect( handle, dragging_ ? QColor( 40, 90, 160 ) : QColor( 70, 70, 80 ) );
    p.setPen( QColor( 165, 165, 175 ) );
    int cx = gx + HANDLE_W/2;
    int cy = height()/2;
    for( int i=-3; i<=3; ++i ) {
        p.drawPoint( cx-2, cy + i*6 );
        p.drawPoint( cx+1, cy + i*6 );
    }

    if( foldable_ ) {
        int fx = depth_*SMV_TRACK_INDENT;
        int midY = SMV_FOLD_W/2 + 2;
        p.setPen( QColor( 60, 60, 60 ) );
        p.setBrush( QColor( 60, 60, 60 ) );
        QPolygon tri;
        if( collapsed_ ) {                      // ▸ collapsed
            tri << QPoint( fx+3, midY-4 ) << QPoint( fx+3, midY+4 ) << QPoint( fx+9, midY );
        } else {                                // ▾ expanded
            tri << QPoint( fx+2, midY-3 ) << QPoint( fx+10, midY-3 ) << QPoint( fx+6, midY+3 );
        }
        p.drawPolygon( tri );
    }
}

void SSMVMixerControl::mousePressEvent( QMouseEvent *ev )
{
    int x = ev->pos().x();
    int fx = depth_*SMV_TRACK_INDENT;
    int gx = gripLeft( depth_ );

    // A left click on the fold triangle toggles this parent's children.
    if( ev->button()==Qt::LeftButton && foldable_ && x>=fx && x<fx+SMV_FOLD_W ) {
        ev->accept();
        // Toggling rebuilds the control column (deletes controls, incl. this
        // one) — do it after this event returns, never inside the handler.
        SStdMixerView *view = &smv_;
        STrack *tk = &tk_;
        QMetaObject::invokeMethod( view, [view, tk]() { view->toggleTrackCollapsed( tk ); },
                                   Qt::QueuedConnection );
        return;
    }
    // A left press on the grip strip arms a track-reorder drag.
    if( ev->button()==Qt::LeftButton && x>=gx && x<gx+HANDLE_W ) {
        // Grabbing the grip of a lane that is ALREADY part of the selection
        // must not collapse it — that is what makes dragging several tracks at
        // once possible. Grabbing any other lane selects it first, so the drag
        // and the highlight agree.
        SStdMixer *mixer = smv_.getModel();
        if( mixer && !mixer->isTrackSelected( &tk_ ) )
            smv_.applyTrackSelectionClick( &tk_, ev->modifiers() );
        dragArmed_ = true;
        dragging_ = false;
        dragPressPos_ = ev->pos();
        ev->accept();
        return;
    }

    // Select this track for the detail panel on click. Plain = just this one,
    // Ctrl = toggle, Shift = extend the lane range (see
    // SStdMixerView::applyTrackSelectionClick).
    if( ev->button()==Qt::LeftButton ) {
        smv_.applyTrackSelectionClick( &tk_, ev->modifiers() );
    }

    QWidget::mousePressEvent( ev );
}

// Right-clicking a head shows the arranger's TRACK menu — the same one the
// timeline canvas offers, so the multi-track operations are reachable from the
// place the multi-selection is made.
void SSMVMixerControl::contextMenuEvent( QContextMenuEvent *ev )
{
    smv_.showTrackContextMenu( &tk_, ev->globalPos() );
    ev->accept();
}

void SSMVMixerControl::mouseMoveEvent( QMouseEvent *ev )
{
    if( dragArmed_ && (ev->buttons() & Qt::LeftButton) ) {
        if( !dragging_ ) {
            if( abs( ev->pos().y()-dragPressPos_.y() ) < DRAG_THRESHOLD ) return;
            dragging_ = true;
            setCursor( Qt::ClosedHandCursor );
            smv_.beginTrackDrag( this );
            update();   // repaint the grip in its "active" colour
        }
        // Report the pointer in the control-column's coordinate space.
        smv_.updateTrackDrag( mapToParent( ev->pos() ).y() );
        ev->accept();
        return;
    }
    QWidget::mouseMoveEvent( ev );
}

void SSMVMixerControl::resizeEvent( QResizeEvent *ev )
{
    QWidget::resizeEvent( ev );
    updateLayout();
}

void SSMVMixerControl::mouseReleaseEvent( QMouseEvent *ev )
{
    if( dragging_ ) {
        dragging_ = false;
        dragArmed_ = false;
        unsetCursor();
        update();
        int y = mapToParent( ev->pos() ).y();
        ev->accept();
        // endTrackDrag can rebuild the control column (deleting controls,
        // including this one). Run it after this event returns so we never
        // mutate/destroy widgets from inside their own event dispatch.
        SStdMixerView *view = &smv_;
        QMetaObject::invokeMethod( view, [view, y]() { view->endTrackDrag( y ); },
                                   Qt::QueuedConnection );
        return;
    }
    dragArmed_ = false;
    QWidget::mouseReleaseEvent( ev );
}

// The tracks this head's toggles act on: the whole selection when this lane is
// part of it, otherwise this lane alone (SStdMixerView::selectionTargets holds
// the rule). Pressing M on an UNSELECTED lane therefore never reaches a lane
// the user is not pointing at.
QList<STrack *> SSMVMixerControl::toggleTargets() const
{
    return smv_.selectionTargets( &tk_ );
}

// Mute and solo go through the action system (path-addressed, so a nested lane
// is nameable) rather than writing the model directly: that is what makes them
// undoable, scriptable — and testable, which is why nested solo could break and
// stay broken. The model write itself is unchanged; setMuted()/setSolo() still
// do the lazy invalidation (proposal 06), so no notifyArrangementChanged() here.
//
// Broadcasting over a selection is ONE undo step (a macro), because the user
// made one gesture. Each target gets its own absolute value — `on`, the state
// the pressed button now shows — so a mixed selection ends up uniform rather
// than inverted lane by lane.
void SSMVMixerControl::muteToggled( bool on )
{
    SStdMixer *mixer = smv_.getModel();
    if( !mixer ) { tk_.setMuted( on ); return; }
    const QList<STrack *> targets = toggleTargets();
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Mute tracks" ) );
    for( STrack *t : targets ) {
        if( t->isMuted() == on ) continue;      // nothing to undo for this one
        stimeline::submitActive(
            new SSetTrackMuteAction( strackpath::pathOf( mixer, t ), on ) );
    }
    if( macro ) stack->endMacro();
}

void SSMVMixerControl::soloToggled( bool on )
{
    SStdMixer *mixer = smv_.getModel();
    if( !mixer ) { tk_.setSolo( on ); return; }
    const QList<STrack *> targets = toggleTargets();
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Solo tracks" ) );
    for( STrack *t : targets ) {
        if( t->isSolo() == on ) continue;
        stimeline::submitActive(
            new SSetTrackSoloAction( strackpath::pathOf( mixer, t ), on ) );
    }
    if( macro ) stack->endMacro();
}

void SSMVMixerControl::onMutedChanged( bool on )
{
    QSignalBlocker block( qMute_ );   // don't bounce back into muteToggled
    qMute_->setChecked( on );
    update();  // Repaint to show/hide gray background
}

void SSMVMixerControl::onSoloChanged( bool on )
{
    QSignalBlocker block( qSolo_ );
    qSolo_->setChecked( on );
    update();  // Repaint to show/hide yellow background
}

// The "G" shortcut (proposal 17 phase 4, decision 4): grouped -> dissolve
// the WHOLE group (every member, wherever it lives); ungrouped -> lock this
// track and its subtree together under a fresh id. One undo macro either
// way. Arbitrary sets beyond the shortcut go through set-edit-group.
void SSMVMixerControl::groupToggled( bool /*checked*/ )
{
    SStdMixer *mixer = smv_.getModel();
    if( !mixer ) return;
    SObject *root = mixer;
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();

    QList<QPair<QList<int>, int>> assignments;   // (trackPath, group id)
    if( tk_.getEditGroup() != 0 ) {
        QList<SObject *> members;
        seditgroups::membersOf( root, tk_.getEditGroup(), members );
        for( SObject *m : members )
            assignments.append( qMakePair( strackpath::pathOf( root, m ), 0 ) );
    } else {
        // A multi-selection locks into ONE group: that is the gesture's whole
        // point, and the per-track shortcut is just its one-element case.
        const int freshId = seditgroups::maxEditGroupId( root ) + 1;
        QList<SObject *> lanes;
        for( STrack *t : toggleTargets() )
            seditgroups::collectSubtreeLanes( t, lanes );
        for( SObject *l : lanes ) {
            const QList<int> p = strackpath::pathOf( root, l );
            bool dup = false;
            for( const auto &a : assignments ) if( a.first == p ) { dup = true; break; }
            if( !dup ) assignments.append( qMakePair( p, freshId ) );
        }
    }

    const bool macro = assignments.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Edit group" ) );
    for( const auto &a : assignments )
        stimeline::submitActive(
            new SSetEditGroupAction( a.first, a.second ) );
    if( macro ) stack->endMacro();
}

void SSMVMixerControl::onEditGroupChanged( int id )
{
    QSignalBlocker block( qGroup_ );
    qGroup_->setChecked( id != 0 );
}

void SSMVMixerControl::takesToggled( bool on )
{
    // View-level UI state; triggers refreshTrackTree(), which rebuilds the
    // control column (this control dies via deleteLater — safe from here).
    // Over the selection like the other toggles, and driven TO the pressed
    // button's state rather than toggled per lane, so a mixed selection ends
    // up uniform. The target list is captured before the first rebuild.
    const QList<STrack *> targets = toggleTargets();
    for( STrack *t : targets ) {
        if( smv_.isTrackTakesExpanded( t ) != on ) smv_.toggleTrackTakesExpanded( t );
    }
}

// ARM IS A VERB SINCE PROPOSAL 21 L1b. It used to be a direct model write on
// the grounds that arming is transport state rather than project state; it is
// now `arm-track`, for two reasons that outweigh that. It decides whether a
// track is MONITORED, which re-wires the mixer and hands a plugin chain to
// another thread - so it has to go through the one place that owns that
// ordering (SAppContext::liveLanesChanged, raised by the verb) - and a user who
// armed the wrong lane of a multi-selection has the same right to undo it as
// one who muted it. Over the selection like the other two.
void SSMVMixerControl::armToggled( bool on )
{
    SStdMixer *mixer = smv_.getModel();
    if( !mixer ) {
        for( STrack *t : toggleTargets() ) t->setArmedForRecording( on );
        return;
    }
    const QList<STrack *> targets = toggleTargets();
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Arm tracks" ) );
    for( STrack *t : targets ) {
        if( t->isArmedForRecording() == on ) continue;
        stimeline::submitActive(
            new SArmTrackAction( strackpath::pathOf( mixer, t ), on ) );
    }
    if( macro ) stack->endMacro();
}

void SSMVMixerControl::onArmedChanged( bool on )
{
    QSignalBlocker block( qArm_ );
    qArm_->setChecked( on );
}

void SSMVMixerControl::commitTrackName()
{
    if( !qTrkLabel_ ) return;
    const QString typed = qTrkLabel_->text();
    if( typed == tk_.getSName() ) return;   // no edit -> no undo entry

    // Deliberately NOT over toggleTargets(): mute/solo/arm apply one boolean to
    // every selected lane, but one typed string on four lanes would just give
    // four identically-named tracks. A name is per-track.
    // editingFinished also fires on the focus-out a head rebuild or a teardown
    // causes, so the model may already be gone; there is nothing to commit
    // against then.
    SStdMixer *mixer = smv_.getModel();
    const QList<int> path = mixer ? strackpath::pathOf( mixer, &tk_ )
                                  : QList<int>();
    if( path.isEmpty() ) {
        // Not reachable from the mixer root (a head outliving its track, say):
        // put the model's name back rather than dropping the edit silently.
        qTrkLabel_->setText( tk_.getSName() );
        return;
    }
    stimeline::submitActive( new SSetTrackNameAction( path, typed ) );
}

SSMVMixerControl::~SSMVMixerControl()
{
    // Deletes all widgets by default
}

SSMVMixerControl::SSMVMixerControl(
    QWidget *parent, SStdMixerView &smv, STrack &tk )
    : QWidget( parent ),
      dragArmed_( false ),
      dragging_( false ),
      smv_( smv ),
      tk_( tk )
{
    qLayout_ = new QGridLayout( this );
    // Reserve the left edge for [fold triangle][grip]; setTreeInfo() widens this
    // by the indent for nested tracks.
    qLayout_->setContentsMargins( SMV_FOLD_W + HANDLE_W, 2, 4, 2 );
    qLayout_->setSpacing( 2 );

    // Small label font derived from the application default (the bundled
    // FreeSans, antialiased) so the channel strip matches the rest of the UI.
    QFont smallFont = QApplication::font();
    smallFont.setPointSize( 9 );

    qTrkLabel_ = new QLineEdit( tk_.getSName(), this );
    qTrkLabel_->setFrame( false );
    qTrkLabel_->setFont( smallFont );
    // A QLineEdit asks for a fair minimum width. Beside the button row that
    // would push the fixed-size buttons out of the head instead of squeezing
    // the name, so let it be squeezed (placeLabel decides when it fits at all).
    qTrkLabel_->setMinimumWidth( 24 );
    // Install event filter to pass mouse clicks through to parent for track selection
    qTrkLabel_->installEventFilter( this );
    // Lose focus when Enter/Return is pressed
    QObject::connect( qTrkLabel_, &QLineEdit::returnPressed,
                      qTrkLabel_, &QLineEdit::clearFocus );
    // ...and COMMIT the typed name. This field used to be wired to nothing at
    // all: it was seeded from the model once and no edit ever reached it, so a
    // rename was not saved, not undoable, and lost the next time the head was
    // rebuilt. editingFinished covers both endings of the gesture — Enter (via
    // the clearFocus above) and clicking away.
    QObject::connect( qTrkLabel_, &QLineEdit::editingFinished,
                      this, &SSMVMixerControl::commitTrackName );
    // Keep the field honest about the model: an undo, a script or any other
    // writer must show up here. Skipped while the user is typing, so a
    // rebuild-triggered refresh cannot eat a half-entered name.
    QObject::connect( &tk_, &SObject::sNameChanged,
                      this, [this]( const QString &n ) {
        if( qTrkLabel_ && !qTrkLabel_->hasFocus() && qTrkLabel_->text() != n )
            qTrkLabel_->setText( n );
    } );

    // Vertical fader, like a channel strip on a console. Works in tenths of a
    // dB; loud at the top (Qt vertical sliders put the maximum at the top).
    qVolume_ = new QSlider( Qt::Vertical, this );
    qVolume_->setRange( FADER_MIN, FADER_MAX );
    qVolume_->setSingleStep( 10 );    // 1.0 dB per arrow key / wheel notch
    qVolume_->setPageStep( 60 );      // 6.0 dB per page
    qVolume_->setTickPosition( QSlider::TicksRight );
    qVolume_->setTickInterval( 120 ); // a tick every 12 dB
    // Watch for a double-click on the fader to reset it to 0.0 dB.
    qVolume_->installEventFilter( this );

    qVolLabel_ = new QLabel( this );
    qVolLabel_->setAlignment( Qt::AlignHCenter );
    qVolLabel_->setFont( smallFont );
    // Set fixed width to prevent layout shifting when digit count changes
    QFontMetrics fm( smallFont );
    int maxWidth = fm.horizontalAdvance( "-96.0 dB" ) + 4;
    qVolLabel_->setFixedWidth( maxWidth );

    // Small square Mute / Solo toggle buttons. Mute (red when on) silences this
    // track; Solo (yellow when on) silences every track that is not soloed.
    // Small square Mute / Solo toggle buttons. A compact bold font keeps the
    // single glyph from being clipped inside the 20x20 button (the default
    // FreeSans size was too tall) while preserving the native button look.
    QFont btnFont = QApplication::font();
    btnFont.setPointSize( 8 );
    btnFont.setBold( true );
    // The DESELECTED state of these buttons sets no background of its own
    // (it keeps the plain native button face), so it used to leave the
    // glyph's colour to whatever the platform/theme default happened to be —
    // low contrast against that face in practice. A `QPushButton { color:… }`
    // base rule fixes only the glyph; a `:checked` rule is more specific in
    // Qt's stylesheet cascade and still wins whenever the button IS engaged,
    // so the active-state background/colour pairs below are unchanged
    // (fix/track-list-polish n).
    static const QString INACTIVE_GLYPH = QStringLiteral(
        "QPushButton { color:#303030; }" );

    qMute_ = new QPushButton( "M", this );
    qMute_->setCheckable( true );
    qMute_->setFixedSize( 20, 20 );
    qMute_->setFont( btnFont );
    qMute_->setToolTip( "Mute" );
    qMute_->setStyleSheet( INACTIVE_GLYPH +
        "QPushButton:checked { background:#d04040; color:white; }" );
    qSolo_ = new QPushButton( "S", this );
    qSolo_->setCheckable( true );
    qSolo_->setFixedSize( 20, 20 );
    qSolo_->setFont( btnFont );
    qSolo_->setToolTip( "Solo" );
    qSolo_->setStyleSheet( INACTIVE_GLYPH +
        "QPushButton:checked { background:#e0c020; color:black; }" );

    qArm_ = new QPushButton( "R", this );
    qArm_->setCheckable( true );
    qArm_->setFixedSize( 20, 20 );
    qArm_->setFont( btnFont );
    qArm_->setToolTip( "Arm for Recording / Monitoring\n"
                       "(Right-click for input, channels and monitor mode)" );
    qArm_->setStyleSheet( INACTIVE_GLYPH +
        "QPushButton:checked { background:#c04040; color:white; }" );
    qArm_->setContextMenuPolicy( Qt::CustomContextMenu );

    qTakes_ = new QPushButton( "T", this );
    qTakes_->setCheckable( true );
    qTakes_->setFixedSize( 20, 20 );
    qTakes_->setFont( btnFont );
    qTakes_->setToolTip( "Show take lanes (takes stack up when you record over a clip)" );
    qTakes_->setStyleSheet( INACTIVE_GLYPH +
        "QPushButton:checked { background:#4080c0; color:white; }" );

    qGroup_ = new QPushButton( "G", this );
    qGroup_->setCheckable( true );
    qGroup_->setFixedSize( 20, 20 );
    qGroup_->setFont( btnFont );
    qGroup_->setToolTip( "Edit group: lock this track (and its subtree) together" );
    qGroup_->setStyleSheet( INACTIVE_GLYPH +
        "QPushButton:checked { background:#40a060; color:white; }" );

    // Proposal 37 6.1 - the second pair. Both are FULL-DENSITY ONLY and both
    // additionally require the button column to still fit vertically: five
    // buttons need 108 px of a 132 px Full lane, and seven need 152, so
    // unconditional buttons would clip exactly the shortest Full lanes.
    qInstr_ = new QPushButton( "I", this );
    qInstr_->setFixedSize( 20, 20 );
    qInstr_->setFont( btnFont );
    qInstr_->setToolTip( "Instrument: open its parameter editor" );
    qInstr_->setStyleSheet( "QPushButton { background:#6050a0; color:white; }" );
    qInstr_->hide();      // no instrument until slot 0 says so

    qAuto_ = new QPushButton( "A", this );
    qAuto_->setFixedSize( 20, 20 );
    qAuto_->setFont( btnFont );
    qAuto_->setContextMenuPolicy( Qt::CustomContextMenu );

    // Mute over Solo over Arm in a column. QBoxLayout (not QVBoxLayout) so the
    // compact density can lay the same buttons out in a row.
    qBtnCol_ = new QBoxLayout( QBoxLayout::TopToBottom );
    qBtnCol_->setContentsMargins( 0, 0, 0, 0 );
    qBtnCol_->setSpacing( 2 );
    qBtnCol_->addWidget( qMute_, 0, Qt::AlignTop );
    qBtnCol_->addWidget( qSolo_, 0, Qt::AlignTop );
    qBtnCol_->addWidget( qArm_, 0, Qt::AlignTop );
    qBtnCol_->addWidget( qTakes_, 0, Qt::AlignTop );
    qBtnCol_->addWidget( qGroup_, 0, Qt::AlignTop );
    qBtnCol_->addWidget( qInstr_, 0, Qt::AlignTop );
    qBtnCol_->addWidget( qAuto_, 0, Qt::AlignTop );
    qBtnCol_->addStretch( 1 );

    // Fader with its dB readout directly beneath it, both centred so they line
    // up as one column.
    qFaderCol_ = new QBoxLayout( QBoxLayout::TopToBottom );
    qFaderCol_->setContentsMargins( 0, 0, 0, 0 );
    qFaderCol_->setSpacing( 1 );
    qFaderCol_->addWidget( qVolume_, 1, Qt::AlignHCenter );
    qFaderCol_->addWidget( qVolLabel_, 0, Qt::AlignHCenter );

    // Level meter (proposal 34), beside the fader in Full and under it in
    // Compact — qStripRow_ flips direction, so one insert covers both. It fits
    // without squeezing anything: at the default 120 px column the content width
    // is 120 - (SMV_FOLD_W + the 8 px resizer handle) - 4 = 92, of which the
    // buttons and the fader/readout column use 20 + 4 + ~55 = 79, leaving ~13 px
    // that the trailing stretch was absorbing. The meter takes 8 + 4 spacing.
    qMeter_ = new SLevelMeter( this );
    // Bind ONCE: the track's twRewire is created only when null
    // (STrack::buildComponents), so its identity is stable for this head's life.
    qMeter_->setMeterLabel( QString() );   // undo the live label, if it was set
    probe_.setTap( tk_.getRootComponent() );
    syncMeterLanes();

    // Fader column beside the meter (they lie down together in Compact).
    qFaderRow_ = new QBoxLayout( QBoxLayout::LeftToRight );
    qFaderRow_->setContentsMargins( 0, 0, 0, 0 );
    qFaderRow_->setSpacing( 4 );
    qFaderRow_->addLayout( qFaderCol_ );
    qFaderRow_->addWidget( qMeter_ );
    qFaderRow_->addStretch( 1 );

    // The track name sits at the top of this column, i.e. next to the M/S/R/T/G
    // buttons rather than on a full-width row of its own above the strip: at the
    // minimal 120 px column there is no horizontal room for the name BESIDE the
    // fader, so it takes the width above it instead.
    qRightCol_ = new QBoxLayout( QBoxLayout::TopToBottom );
    qRightCol_->setContentsMargins( 0, 0, 0, 0 );
    qRightCol_->setSpacing( 2 );
    qRightCol_->addWidget( qTrkLabel_, 0 );
    qRightCol_->addLayout( qFaderRow_, 1 );

    // Mute/Solo column, then the name + fader/meter column.
    qStripRow_ = new QBoxLayout( QBoxLayout::LeftToRight );
    qStripRow_->setContentsMargins( 0, 0, 0, 0 );
    qStripRow_->setSpacing( 4 );
    qStripRow_->addLayout( qBtnCol_, 0 );
    qStripRow_->addLayout( qRightCol_, 1 );

    // The head is placed by the view at exactly its lane's size (which can be
    // small, and differs from lane to lane), so it must be free to take that
    // size: a minimum width only, and a layout that never pushes a minimum
    // height back onto the widget. updateLayout() hides what does not fit.
    setMinimumWidth( SMV_TRACK_CTRL_WIDTH );
    setMinimumHeight( 0 );
    setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Ignored );

    qLayout_->addLayout( qStripRow_, 0, 0 );
    qLayout_->setRowStretch( 0, 1 );
    qLayout_->setSizeConstraint( QLayout::SetNoConstraint );

    // Seed widgets from the current track state.
    setSliderSilently( tk_.getVolume() );
    qMute_->setChecked( tk_.isMuted() );
    qSolo_->setChecked( tk_.isSolo() );
    qArm_->setChecked( tk_.isArmedForRecording() );
    refreshArmTooltip_();
    qTakes_->setChecked( smv_.isTrackTakesExpanded( &tk_ ) );
    qGroup_->setChecked( tk_.getEditGroup() != 0 );
    refreshAutomationButton();

    // Metering: one app-wide tick drives every meter (proposal 34). Connecting
    // per head is deliberate — heads are deleteLater()'d on every
    // refreshTrackTree(), so the connections drop themselves.
    QObject::connect( &SApplication::app(), &SApplication::meterTick,
                      this, &SSMVMixerControl::onMeterTick );
    QObject::connect( &SApplication::app(), &SApplication::meterReset,
                      qMeter_, &SLevelMeter::resetMeter );

    QObject::connect( qVolume_, SIGNAL( valueChanged( int ) ),
                      this, SLOT( sliderValueChanged( int ) ) );
    QObject::connect( &tk_, SIGNAL( volumeChanged( double ) ),
                      this, SLOT( sliderValueChanged( double ) ) );
    QObject::connect( qMute_, SIGNAL( toggled( bool ) ),
                      this, SLOT( muteToggled( bool ) ) );
    QObject::connect( qSolo_, SIGNAL( toggled( bool ) ),
                      this, SLOT( soloToggled( bool ) ) );
    QObject::connect( qArm_, SIGNAL( toggled( bool ) ),
                      this, SLOT( armToggled( bool ) ) );
    QObject::connect( qArm_, SIGNAL( customContextMenuRequested( const QPoint& ) ),
                      this, SLOT( showChannelMenu() ) );
    QObject::connect( qTakes_, SIGNAL( toggled( bool ) ),
                      this, SLOT( takesToggled( bool ) ) );
    QObject::connect( qGroup_, SIGNAL( toggled( bool ) ),
                      this, SLOT( groupToggled( bool ) ) );
    QObject::connect( qInstr_, SIGNAL( clicked() ),
                      this, SLOT( instrumentClicked() ) );
    QObject::connect( qAuto_, SIGNAL( clicked() ),
                      this, SLOT( automationClicked() ) );
    QObject::connect( qAuto_, SIGNAL( customContextMenuRequested( const QPoint& ) ),
                      this, SLOT( showAutomationModeMenu() ) );
    // A write pass ends when the CONTROL is released (Touch commits there;
    // Latch and Write hold to the transport stop) - proposal 37 P6.
    QObject::connect( qVolume_, &QSlider::sliderReleased, this, []() {
        SApplication::app().automationRecorder().releaseControl();
    } );
    QObject::connect( &tk_, SIGNAL( editGroupChanged( int ) ),
                      this, SLOT( onEditGroupChanged( int ) ) );
    QObject::connect( &tk_, SIGNAL( mutedChanged( bool ) ),
                      this, SLOT( onMutedChanged( bool ) ) );
    QObject::connect( &tk_, SIGNAL( soloChanged( bool ) ),
                      this, SLOT( onSoloChanged( bool ) ) );
    QObject::connect( &tk_, SIGNAL( armedForRecordingChanged( bool ) ),
                      this, SLOT( onArmedChanged( bool ) ) );

    // Connect to mixer's track selection changes for highlighting. BOTH
    // signals: the set moves without the primary moving (Ctrl-click on a third
    // lane) and the primary moves without the set changing, and each has to
    // repaint this head.
    SStdMixer *mixer = smv_.getModel();
    if( mixer ) {
        QObject::connect( mixer, SIGNAL( selectedTrackChanged( STrack * ) ),
                          this, SLOT( onSelectedTrackChanged( STrack * ) ) );
        QObject::connect( mixer, SIGNAL( selectedTracksChanged() ),
                          this, SLOT( onSelectionChanged() ) );
        // Seed from the selection as it stands.
        onSelectionChanged();
    }

    // Seed the density from the size we start at, in case the first geometry
    // the view hands us happens to match the default one (no resize event).
    updateLayout();
}

// THE ARM BUTTON'S RIGHT-CLICK MENU IS THE INPUT SELECTOR (proposal 21 L1b,
// design D9). One menu, three sections: which device, which channels of it,
// and whether to monitor. The device and the mask are two halves of ONE
// portable string ("audio:<device>:<mask>") so a project carries what the user
// picked and SSettings carries only the machine-local id; the monitor mode is
// its own verb because it is the thing a performer changes most often.
void SSMVMixerControl::showChannelMenu()
{
    SProject *project = SApplication::app().getCurrentProject();
    if( !project ) {
        qWarning( "No project available" );
        return;
    }

    // Enumerating devices needs no OPEN device — `listDevices()` is documented
    // to work on a never-opened instance (WASAPIInput::listDevices() stands up
    // its own temporary enumerator). Opening one here just to size a menu would
    // be exactly the driver-splash-screen cost `listDevices()` exists to avoid.
    std::unique_ptr<audio::AudioInput> input = audio::createAudioInput();
    if( !input ) {
        qWarning( "No audio input backend available" );
        return;
    }

    QMenu menu( "Input Channels" );
    uint32_t currentSelection = tk_.getRecordingChannels();

    // --- MONITOR MODE, first: it is the switch a performer reaches for.
    {
        QMenu *mon = menu.addMenu( QStringLiteral( "Monitor" ) );
        struct Row { const char *label; STrack::MonitorMode mode; const char *tip; };
        static const Row rows[] = {
            { "Auto (tape machine)", STrack::MonitorMode::Auto,
              "Input while stopped or recording; the track's own material on Play" },
            { "On",  STrack::MonitorMode::On,  "Always monitor the input" },
            { "Off", STrack::MonitorMode::Off, "Never monitor the input" },
        };
        for( const Row &r : rows ) {
            QAction *a = mon->addAction( QString::fromUtf8( r.label ) );
            a->setCheckable( true );
            a->setChecked( tk_.getMonitorMode() == r.mode );
            a->setToolTip( QString::fromUtf8( r.tip ) );
            const STrack::MonitorMode m = r.mode;
            connect( a, &QAction::triggered, this, [this, m]() { setMonitorMode_( m ); } );
        }
    }

    // Picking a device AND its channel(s) in one click is two verbs under the
    // hood — `setTrackInput_` (the device+mask spec) and `setRecordingChannels`
    // (the model's own recordingChannels_, which also re-writes the same mask
    // back onto trackInput once the device is set) — batched into one undo
    // entry so a single menu click is a single Ctrl+Z.
    QUndoStack *undoStack = SApplication::app().actionHistory()->undoStack();
    auto pickDeviceChannels = [this, undoStack]
            ( const QString &device, unsigned trackMask, uint32_t recordingMask ) {
        if( undoStack ) undoStack->beginMacro( QStringLiteral( "Set track input" ) );
        setTrackInput_( device, trackMask );
        setRecordingChannels( recordingMask );
        if( undoStack ) undoStack->endMacro();
    };

    // Fills in one device's Mono/Stereo groups once its channel count is
    // KNOWN — either a WASAPI endpoint (its mix format is free to read) or an
    // ASIO device the Options page has already probed and cached (below).
    auto buildMonoStereo = [this, pickDeviceChannels]
            ( QMenu *monoMenu, QMenu *stereoMenu, const QString &id,
              uint32_t chCount, bool isCurrentDevice, uint32_t currentSelection ) {
        for( uint32_t ch = 0; ch < chCount; ++ch ) {
            QAction *chAction = monoMenu->addAction( QString::number( ch + 1 ) );
            chAction->setCheckable( true );
            chAction->setChecked( isCurrentDevice && currentSelection == (1U << ch) );
            connect( chAction, &QAction::triggered, this, [pickDeviceChannels, id, ch]() {
                pickDeviceChannels( id, 1U << ch, 1U << ch );
            } );
        }
        // Every ADJACENT pair (1+2, 2+3, 3+4, ..., n-1+n), not just the
        // non-overlapping ones — a stereo source is not necessarily wired to
        // the odd/even boundary.
        for( uint32_t pair = 0; pair + 1 < chCount; ++pair ) {
            QString label = QStringLiteral( "%1+%2" ).arg( pair + 1 ).arg( pair + 2 );
            QAction *pairAction = stereoMenu->addAction( label );
            pairAction->setCheckable( true );
            uint32_t pairMask = (1U << pair) | (1U << (pair + 1));
            pairAction->setChecked( isCurrentDevice && currentSelection == pairMask );
            connect( pairAction, &QAction::triggered, this, [pickDeviceChannels, id, pairMask]() {
                pickDeviceChannels( id, pairMask, pairMask );
            } );
        }
    };

    // --- THE INPUT DEVICE. "None" is a real choice and the default: a track
    //     with no input is not a monitoring source however it is armed. Every
    //     other device is its OWN submenu — "All Channels" plus Mono/Stereo
    //     groups sized from THAT DEVICE'S OWN reported channel count, never
    //     from whatever device happens to be open elsewhere.
    {
        QMenu *dev = menu.addMenu( QStringLiteral( "Input device" ) );
        QAction *none = dev->addAction( QStringLiteral( "None" ) );
        none->setCheckable( true );
        none->setChecked( !tk_.hasTrackInput() );
        connect( none, &QAction::triggered, this,
                 [this]() { setTrackInput_( QString(), 0u ); } );
        dev->addSeparator();
        const QString current = tk_.trackInputAudioDevice();
        for( const audio::AudioInputDeviceInfo &d : input->listDevices() ) {
            const QString id   = QString::fromStdString( d.id );
            const QString name = QString::fromStdString( d.name );
            const bool isCurrentDevice = tk_.hasTrackInput() && current == id;

            QMenu *devMenu = dev->addMenu( name.isEmpty() ? id : name );

            QAction *allAct = devMenu->addAction( QStringLiteral( "All Channels" ) );
            allAct->setCheckable( true );
            allAct->setChecked( isCurrentDevice && currentSelection == 0 );
            connect( allAct, &QAction::triggered, this, [pickDeviceChannels, id]() {
                pickDeviceChannels( id, 1u, 0u );
            } );

            devMenu->addSeparator();
            QMenu *monoMenu   = devMenu->addMenu( QStringLiteral( "Mono" ) );
            QMenu *stereoMenu = devMenu->addMenu( QStringLiteral( "Stereo" ) );

            // channels==0 from listDevices() means "not known without opening"
            // (ASIO, proposal 35) — and THIS MENU MUST NEVER OPEN A DRIVER
            // ITSELF: it can pop up mid-session, during playback or a take,
            // where a driver open is unwelcome. Edit -> Options -> Audio is
            // the deliberate "configure this device" gesture that probes an
            // ASIO device once and caches the result; fall back to that cache.
            uint32_t chCount = d.channels;
            if( chCount == 0 ) chCount = SSettings::instance().audioInputChannelCount( id );
            if( chCount > 0 ) {
                buildMonoStereo( monoMenu, stereoMenu, id, chCount,
                                  isCurrentDevice, currentSelection );
            } else {
                QAction *hint = monoMenu->addAction(
                    QStringLiteral( "Channel count unknown — open in Options → Audio" ) );
                hint->setEnabled( false );
            }
        }

        // --- THE COMPUTER KEYBOARD, plus every MIDI port this machine offers
        //     (proposal 21 L2, design D9). `SMidiInputHub::listPorts()` reports
        //     the keyboard FIRST and it gets its own top-level entry here: its
        //     spec is the bare word "keyboard", never "midi:<name>:...", which
        //     is STrack's own special case (trackInputMidiPort()) rather than a
        //     name this menu would have to spell correctly.
        SMidiInputHub *hub = SApplication::app().midiInputHub();
        const std::vector<audio::MidiPortInfo> midiPorts =
            hub ? hub->listPorts() : std::vector<audio::MidiPortInfo>();
        if( !midiPorts.empty() ) dev->addSeparator();
        for( const audio::MidiPortInfo &p : midiPorts ) {
            const QString name = QString::fromStdString( p.name );
            if( QString::fromStdString( p.id ) == QStringLiteral( "keyboard" ) ) {
                QAction *a = dev->addAction(
                    name.isEmpty() ? QStringLiteral( "Keyboard" ) : name );
                a->setCheckable( true );
                a->setChecked( tk_.getTrackInput() == QStringLiteral( "keyboard" ) );
                connect( a, &QAction::triggered, this, [this]() {
                    setTrackInputSpec_( QStringLiteral( "keyboard" ) );
                } );
                continue;
            }
            // A submenu per port, for the channel: "any" (the whole point of a
            // MIDI port over an audio one is that most instruments want every
            // channel) plus 1..16, matching `midi:<port>:<ch|any>`.
            QMenu *portMenu = dev->addMenu( QStringLiteral( "MIDI: %1" ).arg( name ) );
            const bool onThisPort = tk_.trackInputMidiPort() == name;
            QAction *any = portMenu->addAction( QStringLiteral( "Any channel" ) );
            any->setCheckable( true );
            any->setChecked( onThisPort && tk_.trackInputMidiChannel() < 0 );
            connect( any, &QAction::triggered, this, [this, name]() {
                setTrackInputSpec_( QStringLiteral( "midi:%1:any" ).arg( name ) );
            } );
            portMenu->addSeparator();
            for( int ch = 0; ch < 16; ++ch ) {
                QAction *chAct = portMenu->addAction(
                    QStringLiteral( "Channel %1" ).arg( ch + 1 ) );
                chAct->setCheckable( true );
                chAct->setChecked( onThisPort && tk_.trackInputMidiChannel() == ch );
                connect( chAct, &QAction::triggered, this, [this, name, ch]() {
                    setTrackInputSpec_(
                        QStringLiteral( "midi:%1:%2" ).arg( name ).arg( ch ) );
                } );
            }
        }
    }

    // Show menu at cursor position
    menu.exec( QCursor::pos() );
}

// The two live-input verbs, over the SELECTION like every other head toggle.
void SSMVMixerControl::setTrackInput_( const QString &device, unsigned mask )
{
    const QString spec = device.isEmpty()
        ? QStringLiteral( "none" )
        : QStringLiteral( "audio:%1:%2" ).arg( device ).arg( mask, 0, 16 );
    setTrackInputSpec_( spec );
}

// The MIDI/keyboard counterpart (proposal 21 L2, design D9): `spec` is already
// the portable spelling STrack wants ("keyboard" or "midi:<port>:<ch|any>"),
// so unlike setTrackInput_ above there is nothing here to assemble from a
// device id and a mask - just the same submit-over-the-selection macro.
void SSMVMixerControl::setTrackInputSpec_( const QString &spec )
{
    SStdMixer *mixer = smv_.getModel();
    if( !mixer ) return;
    const QList<STrack *> targets = toggleTargets();
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Set track input" ) );
    for( STrack *t : targets )
        stimeline::submitActive(
            new SSetTrackInputAction( strackpath::pathOf( mixer, t ), spec ) );
    if( macro ) stack->endMacro();
    refreshArmTooltip_();
}

void SSMVMixerControl::setMonitorMode_( STrack::MonitorMode mode )
{
    SStdMixer *mixer = smv_.getModel();
    if( !mixer ) return;
    const QList<STrack *> targets = toggleTargets();
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Set monitor mode" ) );
    for( STrack *t : targets ) {
        if( t->getMonitorMode() == mode ) continue;
        stimeline::submitActive(
            new SSetMonitorModeAction( strackpath::pathOf( mixer, t ), mode ) );
    }
    if( macro ) stack->endMacro();
    refreshArmTooltip_();
}

// The tooltip is where the live state is ANNOUNCED, including the one failure a
// user cannot otherwise see: openLive() REFUSES a device whose rate is not the
// project rate, because a ring entry is stamped in PROJECT frames and the RT
// sums it straight into the device buffer. Monitoring then silently does not
// happen, so it is said here as well as logged.
void SSMVMixerControl::refreshArmTooltip_()
{
    QString tip = QStringLiteral(
        "Arm for Recording / Monitoring\n"
        "(Right-click for input, channels and monitor mode)" );
    tip += QStringLiteral( "\nInput: %1" )
               .arg( tk_.hasTrackInput() ? tk_.getTrackInput()
                                         : QStringLiteral( "none" ) );
    tip += QStringLiteral( "\nMonitor: %1" )
               .arg( STrack::monitorModeToString( tk_.getMonitorMode() ) );
    // The input-channel selection is visible HERE without opening the menu
    // (main PR #52): the default is no longer "all channels", so a user who
    // never opens the menu still sees which input is being recorded.
    {
        const uint32_t channels = tk_.getRecordingChannels();
        if( channels == 0 ) tip += QStringLiteral( "\nSelected: All Channels" );
        else {
            QString channelStr;
            for( uint32_t ch = 0; ch < 32; ++ch )
                if( channels & ( 1U << ch ) ) {
                    if( !channelStr.isEmpty() ) channelStr += ", ";
                    channelStr += QString::number( ch + 1 );
                }
            tip += QStringLiteral( "\nSelected: %1" ).arg( channelStr );
        }
    }
    if( SLiveMonitor *mon = SApplication::app().liveMonitor() ) {
        if( mon->rateRefusals() > 0 )
            tip += QStringLiteral( "\nMONITORING OFF: the audio device will not "
                                   "run at the project rate" );
    }
    qArm_->setToolTip( tip );
}

void SSMVMixerControl::setRecordingChannels( uint32_t channels )
{
    // Like ARM itself, this follows the selection: picking the input channels
    // for one armed lane out of several selected ones is never what was meant.
    for( STrack *t : toggleTargets() ) t->setRecordingChannels( channels );

    // And the same mask is the second half of `trackInput` (design D9): the
    // recording path and the monitoring path must not be able to disagree about
    // which channels of the device this track is listening to.
    if( tk_.hasTrackInput() )
        setTrackInput_( tk_.trackInputAudioDevice(), channels ? channels : 1u );

    refreshArmTooltip_();
}

// Highlight follows the selection SET, not just the primary: with several
// tracks selected every one of their heads is lit, which is the only way the
// user can see what a broadcast toggle is about to hit. `track` is unused —
// this is wired to both selection signals.
void SSMVMixerControl::onSelectedTrackChanged( STrack * )
{
    onSelectionChanged();
}

void SSMVMixerControl::onSelectionChanged()
{
    SStdMixer *mixer = smv_.getModel();
    const bool wasSelected = selected_;
    const bool wasPrimary  = primary_;
    selected_ = mixer && mixer->isTrackSelected( &tk_ );
    primary_  = mixer && mixer->getSelectedTrack() == &tk_;
    if( wasSelected != selected_ || wasPrimary != primary_ ) {
        update();  // Repaint to update background color
    }
}

// Proposal 34 — one metering tick for this track's meter.
void SSMVMixerControl::onMeterTick( offset_t pos, qint64 nowMs, bool live )
{
    // The fader's READ display (proposal 37 P6) runs first and unconditionally:
    // it is not the meter's business and it must keep working on a lane whose
    // meter is hidden by the density rules.
    pumpReadValue( pos );

    // A hidden meter does ZERO work: this is the first and cheapest layer of the
    // repaint-storm defence (30 heads x 30 Hz), and in Tiny density every meter
    // in the project is hidden.
    if( !qMeter_ || !qMeter_->isVisible() ) return;

    // Meters follow mute/solo. This is already EMERGENT — an inaudible track has
    // its mixer input plug nulled (SStdMixer::reconnectTracksToMixer), so nothing
    // pulls or freezes its pages and the mute's epoch bump stales the old ones —
    // but the pages are the TRACK's own output and can legitimately still hold
    // audio (another consumer may have frozen them). Check the model too, so the
    // result does not depend on which path won: THREADING rule 4.
    if( !live ) { qMeter_->pushIdle( nowMs ); return; }

    // THE shared rule (app/model/ssolorules.h) — the same one the mixer routes
    // by. Spelling it out here a second time is how the meter and the ear got to
    // disagree about nested lanes.
    if( !ssolo::isLaneAudible( smv_.getModel(), &tk_ ) ) {
        qMeter_->pushIdle( nowMs );
        return;
    }

    // A LIVE-OWNED track has NO FROZEN PAGES to read (proposal 21 L1b): its
    // audio is rendered by the pump and its chain is excluded from the frozen
    // sum, so the probe would MISS every tick and the bar would decay to the
    // floor while the performer is playing. The head therefore shows the
    // PRE-FX INPUT level, which is the one thing measurable from the main
    // thread, and the meter's label says so. A post-FX live meter would need
    // the pump to publish levels by position - engine work, and not this phase.
    if( tk_.isLiveOwnedLane() ) {
        if( SLiveMonitor *mon = SApplication::app().liveMonitor() ) {
            const double peak = mon->takeInputPeak( &tk_ );
            twLevelSample ls;
            ls.peak       = (float) peak;
            // No RMS is available without keeping a window on the pump thread,
            // and a wrong one would read as a quiet signal. Report the peak as
            // both and let the bar be a peak bar: honest, if coarse.
            ls.meanSquare = (float) ( peak * peak );
            ls.frames     = 1;
            ls.clipped    = peak >= 1.0;
            qMeter_->setMeterLabel(
                QStringLiteral( "Input level (pre-FX) - this track is MONITORED, "
                                "so it has no frozen pages to read" ) );
            qMeter_->pushLevel( ls, nowMs );
            return;
        }
    }

    // Re-bind every tick rather than only in the ctor: setTap() is a pointer
    // compare that no-ops when unchanged, and it makes the meter self-healing if
    // the track's components are ever rebuilt under it — a meter silently dead
    // because it bound before its tap existed is a whole bug class avoided for
    // one comparison.
    probe_.setTap( tk_.getRootComponent() );

    const int shown = syncMeterLanes();

    twLevelSampleSet s;
    if( probe_.advanceTo( pos, s, shown ) ) qMeter_->pushLevel( s, nowMs );
    else                                    qMeter_->pushIdle( nowMs );
}

// Proposal 36 B8 — how many lanes this head's meter draws.
//
// The width is the TAP's declared width. For a track that IS the project's,
// since B4 built one twTrackMix / twPluginChain / twRewire of the project's
// width rather than a per-track bus count; asking the component rather than
// SProject keeps the meter and the audio reading one number, and it is the same
// number §4.5's width-mismatch rule compares a cached page against.
//
// The drawn count is capped at SLevelMeter::MONITOR_LANES — see the constant for
// why the head follows the device rule rather than growing.
int SSMVMixerControl::syncMeterLanes()
{
    std::shared_ptr<twComponent> tap = tk_.getRootComponent();
    const int width = tap ? (int) tap->getOutputChannels() : 1;
    const int shown = qMin( width > 0 ? width : 1, SLevelMeter::MONITOR_LANES );
    if( qMeter_ ) qMeter_->setLanes( shown, width );   // no-op when unchanged
    return shown;
}

int SSMVMixerControl::tkPushMeterLevel( const twLevelSampleSet &s, qint64 nowMs )
{
    const int shown = syncMeterLanes();
    updateLayout();
    if( qMeter_ ) qMeter_->pushLevel( s, nowMs );
    return shown;
}

bool SSMVMixerControl::tkClickToggle( const QString &which, bool on )
{
    QPushButton *b = nullptr;
    if(      which == QStringLiteral( "mute" ) )  b = qMute_;
    else if( which == QStringLiteral( "solo" ) )  b = qSolo_;
    else if( which == QStringLiteral( "arm" ) )   b = qArm_;
    else if( which == QStringLiteral( "takes" ) ) b = qTakes_;
    else if( which == QStringLiteral( "group" ) ) b = qGroup_;
    if( !b ) return false;
    // click() and not setChecked(): the point is to go through the button's
    // own toggled() signal, which is the wire the broadcast hangs off.
    if( b->isChecked() != on ) b->click();
    return true;
}

bool SSMVMixerControl::tkSendFaderKey( const QString &key )
{
    Qt::Key k = Qt::Key_unknown;
    if(      key == QStringLiteral( "Home" ) ) k = Qt::Key_Home;
    else if( key == QStringLiteral( "End" ) )  k = Qt::Key_End;
    else return false;
    // QApplication::sendEvent runs the exact same dispatch a real keystroke
    // would (installed event filters first, then the target's own event()),
    // so this reaches eventFilter() above precisely as focus + a keystroke
    // would — without needing real OS-level focus under QT_QPA_PLATFORM=offscreen.
    QKeyEvent ev( QEvent::KeyPress, k, Qt::NoModifier );
    QApplication::sendEvent( qVolume_, &ev );
    return true;
}

bool SSMVMixerControl::hasInstrumentSlot() const
{
    SPluginChain *chain = tk_.getPluginChain();
    if( !chain || chain->getSlotCount() <= 0 ) return false;
    SPluginSlot *slot = chain->getSlotAt( 0 );
    // The DESCRIPTOR's flag, not the live plugin's: a slot whose plugin is
    // missing on this machine keeps its identity (plugins/CONTRACT), and the
    // head must still show that the track is an instrument track.
    return slot && slot->getDescriptor().isInstrument;
}

void SSMVMixerControl::instrumentClicked()
{
    SPluginChain *chain = tk_.getPluginChain();
    SPluginSlot *slot = chain ? chain->getSlotAt( 0 ) : nullptr;
    if( !slot ) return;

    SStdMixer *mixer = smv_.getModel();
    SObject *root = mixer ? static_cast<SObject *>( mixer ) : nullptr;
    if( !root ) return;
    const QString trackPath =
        strackpath::pathToString( strackpath::pathOf( root, &tk_ ) );

    // The generic parameter editor, as its own window - the same widget the FX
    // strip opens on a double-click, so a parameter edit is the same undoable
    // set-plugin-param either way (pluginui/CONTRACT inv. 3). The native
    // editor is proposal 33 M3.
    SPluginParamEditor *editor =
        new SPluginParamEditor( slot, trackPath, 0, nullptr );
    editor->setAttribute( Qt::WA_DeleteOnClose );
    editor->setWindowTitle( tr( "%1 - %2" )
        .arg( tk_.getSName(), slot->getDescriptor().name.empty()
                                  ? tr( "Instrument" )
                                  : QString::fromStdString(
                                        slot->getDescriptor().name ) ) );
    editor->resize( 340, 420 );
    editor->show();
}

QString SSMVMixerControl::describeHead()
{
    // Apply the density rules for the CURRENT size before describing them: Qt
    // delivers no resizeEvent to a widget that was never shown, so a headless
    // caller's resize() alone would describe the previous layout. Same reason
    // as describeMeter().
    updateLayout();

    struct Row { QPushButton *b; const char *tag; };
    const Row rows[] = { { qMute_, "M" }, { qSolo_, "S" }, { qArm_, "R" },
                         { qTakes_, "T" }, { qGroup_, "G" },
                         { qInstr_, "I" }, { qAuto_, "A" } };
    QStringList visible;
    int nVisible = 0;
    for( const Row &r : rows )
        if( r.b && !r.b->isHidden() ) { visible << QLatin1String( r.tag ); ++nVisible; }

    const int btn = ( density_ == Density::Full ) ? 20 : 16;
    const QMargins m = qLayout_->contentsMargins();
    const int contentW = width() - m.left() - m.right();
    const int contentH = height() - m.top() - m.bottom();
    const bool columnVertical =
        ( qBtnCol_->direction() == QBoxLayout::TopToBottom );
    // "Hiding beats clipping" made assertable: the VISIBLE strip has to fit the
    // lane it was given, in the direction the buttons are laid out.
    const int run = nVisible > 0 ? nVisible * btn + ( nVisible - 1 ) * 2 : 0;
    const bool fitH = columnVertical ? ( contentH >= run )
                                     : ( nVisible == 0 || contentH >= btn );
    const bool fitW = columnVertical ? ( nVisible == 0 || contentW >= btn )
                                     : ( contentW >= run );

    const char *dens = density_ == Density::Full ? "Full"
                     : density_ == Density::Compact ? "Compact" : "Tiny";
    const char *spot = labelSpot_ == LabelSpot::BesideButtons ? "beside"
                     : labelSpot_ == LabelSpot::InButtonRow ? "inrow" : "ownline";

    // `Amode` is the MODE GLYPH (proposal 37 P6): the track's automation mode,
    // reported at EVERY density - the button hides on a short lane, the mode
    // does not stop existing. Appended after `name=` on purpose, so every
    // committed `contains=` written against the P4 string still matches.
    return QStringLiteral( "density=%1|w=%2|h=%3|btns=%4|I=%5|A=%6"
                           "|fitW=%7|fitH=%8|name=%9|Amode=%10" )
        .arg( QLatin1String( dens ) ).arg( width() ).arg( height() )
        .arg( visible.join( QLatin1Char( ',' ) ) )
        .arg( ( qInstr_ && !qInstr_->isHidden() ) ? 1 : 0 )
        .arg( ( qAuto_ && !qAuto_->isHidden() ) ? 1 : 0 )
        .arg( fitW ? 1 : 0 ).arg( fitH ? 1 : 0 )
        .arg( QLatin1String( spot ) )
        .arg( sAutomationModeToString( trackAutomationMode() ) );
}

QString SSMVMixerControl::describeMeter()
{
    if( !qMeter_ ) return QStringLiteral( "vis=0" );
    // Apply the density rules for the CURRENT size before describing them: Qt
    // does not deliver a resizeEvent to a widget that was never shown, so a
    // headless caller's resize() alone would describe the previous layout.
    // Same reason for the lane sync: a headless head never receives a meterTick.
    syncMeterLanes();
    updateLayout();
    return qMeter_->describe();
}

SSMVMixerControl::Density SSMVMixerControl::densityFor( int h ) const
{
    if( h >= DENSITY_FULL_MIN_H )    return Density::Full;
    if( h >= DENSITY_COMPACT_MIN_H ) return Density::Compact;
    return Density::Tiny;
}

// The head is sized to its lane, and lanes are individually sized — so the
// strip reshapes itself to whatever it is given rather than overflowing its
// widget (which Qt would simply clip, the "squashed head" symptom).
void SSMVMixerControl::applyDensity( Density d )
{
    const int btn = ( d == Density::Full ) ? 20 : 16;
    for( QPushButton *b : { qMute_, qSolo_, qArm_, qTakes_, qGroup_,
                            qInstr_, qAuto_ } )
        b->setFixedSize( btn, btn );

    // The second button pair (proposal 37 6.1): Full density only, and only
    // while the COLUMN still fits. "I" additionally needs slot 0 to actually be
    // an instrument - a button that opens nothing is worse than no button.
    const bool showAuto  = ( d == Density::Full ) && buttonColumnFits( btn, 6 );
    const bool showInstr = ( d == Density::Full ) && hasInstrumentSlot()
                         && buttonColumnFits( btn, showAuto ? 7 : 6 );
    qAuto_->setVisible( showAuto );
    qInstr_->setVisible( showInstr );

    switch( d ) {
    case Density::Full:
        // Buttons in a column beside the name + tall fader with its dB readout.
        qBtnCol_->setDirection( QBoxLayout::TopToBottom );
        qStripRow_->setDirection( QBoxLayout::LeftToRight );
        qFaderRow_->setDirection( QBoxLayout::LeftToRight );
        qMute_->show(); qSolo_->show();
        qArm_->show(); qTakes_->show(); qGroup_->show();
        qVolume_->show();
        qVolLabel_->show();
        qMeter_->show();
        placeLabel( LabelSpot::BesideButtons );
        break;
    case Density::Compact: {
        // Small buttons beside a horizontal fader; the dB readout only survives
        // while there is room for another row.
        qFaderRow_->setDirection( QBoxLayout::TopToBottom );
        qMute_->show(); qSolo_->show();
        qArm_->show(); qTakes_->show(); qGroup_->show();
        qVolume_->show();
        qVolLabel_->setVisible( height() >= 84 );
        // The meter is the first thing to go when the rows stack up: a fader you
        // cannot see is worse than a meter you cannot see.
        qMeter_->setVisible( height() >= 60 );
        // The buttons stay a COLUMN as long as one fits vertically — that is
        // what leaves the name a place beside them. Five 16 px buttons need 92
        // px of the ~96 a default 100 px lane has; below that they lie down into
        // a row, which fills the minimal 120 px column all by itself and pushes
        // the name back onto a line of its own.
        if( buttonColumnFits( btn, 5 ) ) {
            qBtnCol_->setDirection( QBoxLayout::TopToBottom );
            qStripRow_->setDirection( QBoxLayout::LeftToRight );
            placeLabel( LabelSpot::BesideButtons );
        } else {
            qBtnCol_->setDirection( QBoxLayout::LeftToRight );
            qStripRow_->setDirection( QBoxLayout::TopToBottom );
            placeLabel( nameFitsInButtonRow( btn ) ? LabelSpot::InButtonRow
                                                   : LabelSpot::OwnLine );
        }
        break;
    }
    case Density::Tiny:
        // Barely a lane: the name, and Mute/Solo beside it while a single
        // button row still fits under it. Hiding beats clipping — a
        // half-drawn fader reads as a rendering bug.
        qBtnCol_->setDirection( QBoxLayout::LeftToRight );
        qStripRow_->setDirection( QBoxLayout::TopToBottom );
        qFaderRow_->setDirection( QBoxLayout::TopToBottom );
        qArm_->hide(); qTakes_->hide(); qGroup_->hide();
        qMute_->setVisible( height() >= 38 );
        qSolo_->setVisible( height() >= 38 );
        qVolume_->hide();
        qVolLabel_->hide();
        qMeter_->hide();
        // Only M/S here, so the name usually does fit beside them — which is the
        // whole strip on a lane this short.
        placeLabel( nameFitsInButtonRow( btn ) ? LabelSpot::InButtonRow
                                               : LabelSpot::OwnLine );
        break;
    }

    // The fader lies down whenever it is short of vertical room (or the column
    // is wide enough to make a horizontal fader the nicer shape).
    const bool horizontalFader = wideMode_ || ( d != Density::Full );
    qVolume_->setOrientation( horizontalFader ? Qt::Horizontal : Qt::Vertical );
    qVolume_->setMinimumHeight( horizontalFader ? 16 : 40 );
    qVolume_->setMaximumHeight( horizontalFader ? 20 : QWIDGETSIZE_MAX );

    // The meter lies down with the fader so the two read as one control.
    qMeter_->setOrientation( horizontalFader ? Qt::Horizontal : Qt::Vertical );

    // In Tiny the name is the whole strip; keep it off the grip and readable.
    qTrkLabel_->setVisible( height() >= 14 );
}

// Move the name to the spot the current shape of the strip calls for. A no-op
// when it is already there, because applyDensity() runs on every resize and
// re-inserting a widget on each pixel of a drag would thrash the layout.
void SSMVMixerControl::placeLabel( LabelSpot spot )
{
    if( labelSpot_ == spot ) return;
    // removeWidget() is a no-op on the layouts that do not hold it.
    qRightCol_->removeWidget( qTrkLabel_ );
    qStripRow_->removeWidget( qTrkLabel_ );
    qBtnCol_->removeWidget( qTrkLabel_ );

    switch( spot ) {
    case LabelSpot::BesideButtons:
        qRightCol_->insertWidget( 0, qTrkLabel_, 0 );
        break;
    case LabelSpot::InButtonRow:
        // Just before the trailing stretch, centred against the small buttons.
        qBtnCol_->insertWidget( qBtnCol_->count()-1, qTrkLabel_, 1,
                                Qt::AlignVCenter );
        break;
    case LabelSpot::OwnLine:
        qStripRow_->insertWidget( 0, qTrkLabel_, 0 );
        break;
    }
    // The name takes the row's slack when it is in it; otherwise the trailing
    // stretch does (that is what keeps the buttons packed to the top/left).
    qBtnCol_->setStretch( qBtnCol_->count()-1,
                          spot == LabelSpot::InButtonRow ? 0 : 1 );
    labelSpot_ = spot;
}

// Does a column of `nBtns` buttons of this size still fit the lane's height?
// Below that the buttons have to lie down into a row.
bool SSMVMixerControl::buttonColumnFits( int btn, int nBtns ) const
{
    const QMargins m = qLayout_->contentsMargins();
    return height() - m.top() - m.bottom() >= nBtns*btn + (nBtns-1)*2;
}

// Does a name field wide enough to read still fit beside the visible buttons?
// Purely a width question: the button row is fixed-size, so what is left over is
// what the name would get.
bool SSMVMixerControl::nameFitsInButtonRow( int btn ) const
{
    int nBtns = 0;
    for( QPushButton *b : { qMute_, qSolo_, qArm_, qTakes_, qGroup_,
                            qInstr_, qAuto_ } )
        if( !b->isHidden() ) ++nBtns;
    if( nBtns == 0 ) return false;      // no row to ride on
    const QMargins m = qLayout_->contentsMargins();
    int content = width() - m.left() - m.right();
    int row = nBtns*btn + (nBtns-1)*2;  // qBtnCol_ spacing is 2
    return content - row - 2 >= NAME_MIN_W;
}

void SSMVMixerControl::updateLayout()
{
    // Applied on every resize, not only on a mode flip: some rules (the dB
    // readout) depend on the height *within* a density. Every setter below
    // no-ops when the value is unchanged, so this stays cheap during a drag.
    wideMode_ = width() > WIDE_MODE_THRESHOLD;
    density_  = densityFor( height() );
    applyDensity( density_ );
}

// ---------------------------------------------------------------------------
// The "A" button: the track's automation MODE (proposal 37 P6, design D5/6.1)
// ---------------------------------------------------------------------------
//
// One button, one mode, EVERY lane the track owns. See the declaration in the
// header for why that is the only unambiguous reading of a single head button.

namespace {

// One addressable lane of `track`: its own `self:` lanes plus every `param:`
// lane on every plugin slot in its chain.
struct LaneAddr {
    QString target;
    int     slotIndex = -1;
    SAutomationLane *lane = nullptr;
};

QList<LaneAddr> lanesOfTrack( STrack &tk )
{
    QList<LaneAddr> out;
    const QList<SAutomationLane *> own = tk.automationLanes();
    for( SAutomationLane *l : own ) out.append( LaneAddr{ l->target(), -1, l } );
    if( SPluginChain *chain = tk.getPluginChain() ) {
        for( int i = 0; i < chain->getSlotCount(); ++i ) {
            SPluginSlot *slot = chain->getSlotAt( i );
            if( !slot ) continue;
            const QList<SAutomationLane *> sl = slot->automationLanes();
            for( SAutomationLane *l : sl )
                out.append( LaneAddr{ l->target(), i, l } );
        }
    }
    return out;
}

// The six modes in cycle order. Off first, so a fresh track's button reads
// "nothing is being consumed" and the first click turns Trim on.
const SAutomationMode kModeCycle[] = {
    SAutomationMode::Off,  SAutomationMode::Trim,  SAutomationMode::Read,
    SAutomationMode::Touch, SAutomationMode::Latch, SAutomationMode::Write
};

}  // namespace

SAutomationMode SSMVMixerControl::trackAutomationMode() const
{
    const QList<LaneAddr> lanes = lanesOfTrack( const_cast<STrack &>( tk_ ) );
    if( lanes.isEmpty() ) return SAutomationMode::Off;
    const SAutomationMode first = lanes.first().lane->mode();
    for( const LaneAddr &a : lanes )
        if( a.lane->mode() != first ) return first;   // they disagree: report
                                                      // the first, which is the
                                                      // track's own Volume lane
                                                      // whenever there is one
    return first;
}

void SSMVMixerControl::setTrackAutomationMode( SAutomationMode m )
{
    SStdMixer *mixer = smv_.getModel();
    if( !mixer ) return;
    const QList<int> trackPath = strackpath::pathOf( mixer, &tk_ );
    if( trackPath.isEmpty() ) return;

    QList<LaneAddr> lanes = lanesOfTrack( tk_ );
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();

    if( lanes.isEmpty() ) {
        // Nothing to set a mode ON. Create the lane the head's own control
        // writes to, so the button is never a silent no-op.
        stimeline::submitActive(
            new SAddAutomationLaneAction( trackPath,
                                          QStringLiteral( "self:Volume" ), m,
                                          -1, -1,
                                          std::vector<SAutomationPoint>(),
                                          QString() ) );
        refreshAutomationButton();
        return;
    }

    // ONE undo step for one gesture (timeline inv. 12's rule, applied to the
    // lanes of a track rather than to a selection of tracks).
    const bool macro = ( lanes.size() > 1 ) && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Automation mode" ) );
    for( const LaneAddr &a : lanes )
        stimeline::submitActive(
            new SSetAutomationModeAction( trackPath, a.target, m,
                                          a.slotIndex, -1 ) );
    if( macro ) stack->endMacro();
    refreshAutomationButton();
}

void SSMVMixerControl::automationClicked()
{
    const SAutomationMode cur = trackAutomationMode();
    int i = 0;
    for( int k = 0; k < 6; ++k ) if( kModeCycle[k] == cur ) { i = k; break; }
    setTrackAutomationMode( kModeCycle[( i + 1 ) % 6] );
}

void SSMVMixerControl::showAutomationModeMenu()
{
    QMenu menu;
    const SAutomationMode cur = trackAutomationMode();
    for( SAutomationMode m : kModeCycle ) {
        QString label = sAutomationModeToString( m );
        if( !label.isEmpty() ) label[0] = label[0].toUpper();
        QAction *a = menu.addAction( label );
        a->setCheckable( true );
        a->setChecked( m == cur );
        QObject::connect( a, &QAction::triggered, &menu,
                          [this, m]() { setTrackAutomationMode( m ); } );
    }
    menu.exec( QCursor::pos() );
}

void SSMVMixerControl::refreshAutomationButton()
{
    if( !qAuto_ ) return;
    const SAutomationMode m = trackAutomationMode();
    // The letter stays "A" at every density - three of the six modes start
    // with a letter another button already uses (R/T/L/W vs Arm/Takes), and a
    // 20 px square is not the place to introduce that ambiguity. The MODE is
    // carried by colour and by the tooltip, and it is `Amode=` in
    // describeHead(), which is what a test reads.
    static const struct { SAutomationMode m; const char *css; } kPaint[] = {
        { SAutomationMode::Off,   "background:#3a3a3a; color:#909090;" },
        { SAutomationMode::Trim,  "background:#4a5a6a; color:white;" },
        { SAutomationMode::Read,  "background:#3070b0; color:white;" },
        { SAutomationMode::Touch, "background:#b08030; color:white;" },
        { SAutomationMode::Latch, "background:#a05010; color:white;" },
        { SAutomationMode::Write, "background:#c02020; color:white;" },
    };
    const char *css = kPaint[0].css;
    for( const auto &k : kPaint ) if( k.m == m ) { css = k.css; break; }
    qAuto_->setStyleSheet( QStringLiteral( "QPushButton { %1 }" )
                               .arg( QLatin1String( css ) ) );
    qAuto_->setToolTip( tr( "Automation mode: %1\n"
                            "Click to cycle, right-click to pick.\n"
                            "Applies to every automation lane on this track." )
                            .arg( sAutomationModeToString( m ) ) );
}

// ---------------------------------------------------------------------------
// The fader shows the READ value while a Read-family Volume lane exists
// ---------------------------------------------------------------------------

void SSMVMixerControl::pumpReadValue( offset_t pos )
{
    SAutomationLane *lane = tk_.automationLane( QStringLiteral( "self:Volume" ) );
    if( !lane || !SAutomationRecorder::isReadFamily( lane->mode() ) ) {
        lastReadDb_ = 1e30;
        return;
    }
    // A pass being RECORDED on this very lane must show the hand, not the
    // curve - otherwise the fader fights the finger that is dragging it.
    SAutomationRecorder::Target t;
    SStdMixer *mixer = smv_.getModel();
    t.ownerPath = mixer ? strackpath::pathOf( mixer, &tk_ ) : QList<int>();
    t.target = QStringLiteral( "self:Volume" );
    if( SApplication::app().automationRecorder().isRecording( t ) ) return;

    const double db = lane->valueAt( pos );
    if( qAbs( db - lastReadDb_ ) < 0.05 ) return;    // sub-slider-step: nothing
                                                     // would move on screen
    lastReadDb_ = db;
    applyingReadValue_ = true;
    setSliderSilently( db );
    applyingReadValue_ = false;
}
