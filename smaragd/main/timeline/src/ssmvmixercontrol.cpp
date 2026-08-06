
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

#include "app/shell/sapplication.h"
#include "tw/devices/audio_input.h"
#include "app/shell/ssettings.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/timeline/sstdmixerview.h"
#include "app/timeline/slevelmeter.h"
#include "app/timeline/sfadercurve.h"
#include "app/objects/track/strack.h"
#include "app/model/slink.h"
#include "app/model/sobjectrenderer.h"
#include "app/model/sproject.h"
#include "app/timeline/ssmvmixercontrol.h"
#include "app/objects/track/strackcolormodifier.h"
#include "app/objects/track/ssettrackvolumeaction.h"
#include "app/objects/track/seteditgroupaction.h"
#include "app/model/seditgroups.h"
#include "app/model/sobjectpath.h"
#include "app/actions/sactionhistory.h"
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
 * undoable and (once the engine drain goes async) coalescable. Resolve our
 * track's index in the mixer; if we can't, fall back to a direct mutation so the
 * UI never wedges.
 */
void SSMVMixerControl::applyVolume_( double newVolume )
{
    int trackIdx = trackIndex_();
    if( trackIdx >= 0 ) {
        SApplication::app().submitAction(
            new SSetTrackVolumeAction( trackIdx, newVolume ) );
    } else {
        tk_.setVolume( newVolume );
        // Direct mutation doesn't go through actions, so invalidate caches manually
        // (asset captures need to reflect volume changes).
        if( SProject *p = SApplication::app().getCurrentProject() )
            p->notifyArrangementChanged();
    }
}

/**
 * Double-clicking the fader resets it to unity gain (0.0 dB). We intercept the
 * event here rather than subclass QSlider; the slider would otherwise treat the
 * second press as the start of another drag.
 *
 * Also intercept mouse presses on the track label to allow selecting the track
 * from anywhere on the control (d).
 */
bool SSMVMixerControl::eventFilter( QObject *watched, QEvent *ev )
{
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
            SStdMixer *mixer = smv_.getModel();
            if( mixer ) {
                mixer->setSelectedTrack( &tk_ );
            }
            return false;  // Let the label still handle it for editing
        }
    }
    return QWidget::eventFilter( watched, ev );
}

/**
 * Locate this control's track within the mixer model. Returns -1 if not found
 * (e.g. the track was removed). The action layer keys off this index.
 */
int SSMVMixerControl::trackIndex_() const
{
    SStdMixer *mixer = smv_.getModel();
    if( !mixer ) return -1;

    int n = mixer->getNTracks();
    for( int i = 0; i < n; ++i ) {
        SLink *link = mixer->getTrackAt( i );
        if( link && &link->getSObject() == &tk_ ) {
            return i;
        }
    }
    return -1;
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
    // Determine base color (depends on selection state)
    QColor baseColor = selected_ ? QColor( 64, 100, 140 ) : QColor( 48, 70, 100 );

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
        dragArmed_ = true;
        dragging_ = false;
        dragPressPos_ = ev->pos();
        ev->accept();
        return;
    }

    // Select this track for the detail panel on click
    if( ev->button()==Qt::LeftButton ) {
        SStdMixer *mixer = smv_.getModel();
        if( mixer ) {
            mixer->setSelectedTrack( &tk_ );
        }
    }

    QWidget::mousePressEvent( ev );
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

void SSMVMixerControl::muteToggled( bool on )
{
    tk_.setMuted( on );
    // Lazy invalidation (proposal 06): setMuted() calls notifyDependentsChanged()
    // to invalidate only affected cuts' Playback+Metadata aspects, not the entire
    // arrangement. No need to call notifyArrangementChanged() here.
}

void SSMVMixerControl::soloToggled( bool on )
{
    tk_.setSolo( on );
    // Lazy invalidation (proposal 06): setSolo() calls notifyDependentsChanged()
    // to invalidate only affected cuts' Playback+Metadata aspects, not the entire
    // arrangement. No need to call notifyArrangementChanged() here.
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
        const int freshId = seditgroups::maxEditGroupId( root ) + 1;
        QList<SObject *> lanes;
        seditgroups::collectSubtreeLanes( &tk_, lanes );
        for( SObject *l : lanes )
            assignments.append(
                qMakePair( strackpath::pathOf( root, l ), freshId ) );
    }

    const bool macro = assignments.size() > 1 && stack;
    if( macro ) stack->beginMacro( QStringLiteral( "Edit group" ) );
    for( const auto &a : assignments )
        SApplication::app().submitAction(
            new SSetEditGroupAction( a.first, a.second ) );
    if( macro ) stack->endMacro();
}

void SSMVMixerControl::onEditGroupChanged( int id )
{
    QSignalBlocker block( qGroup_ );
    qGroup_->setChecked( id != 0 );
}

void SSMVMixerControl::takesToggled( bool /*checked*/ )
{
    // View-level UI state; triggers refreshTrackTree(), which rebuilds the
    // control column (this control dies via deleteLater — safe from here).
    smv_.toggleTrackTakesExpanded( &tk_ );
}

void SSMVMixerControl::armToggled( bool on )
{
    tk_.setArmedForRecording( on );
}

void SSMVMixerControl::onArmedChanged( bool on )
{
    QSignalBlocker block( qArm_ );
    qArm_->setChecked( on );
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
    // Install event filter to pass mouse clicks through to parent for track selection
    qTrkLabel_->installEventFilter( this );
    // Lose focus when Enter/Return is pressed
    QObject::connect( qTrkLabel_, &QLineEdit::returnPressed,
                      qTrkLabel_, &QLineEdit::clearFocus );

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
    qMute_ = new QPushButton( "M", this );
    qMute_->setCheckable( true );
    qMute_->setFixedSize( 20, 20 );
    qMute_->setFont( btnFont );
    qMute_->setToolTip( "Mute" );
    qMute_->setStyleSheet( "QPushButton:checked { background:#d04040; color:white; }" );
    qSolo_ = new QPushButton( "S", this );
    qSolo_->setCheckable( true );
    qSolo_->setFixedSize( 20, 20 );
    qSolo_->setFont( btnFont );
    qSolo_->setToolTip( "Solo" );
    qSolo_->setStyleSheet( "QPushButton:checked { background:#e0c020; color:black; }" );

    qArm_ = new QPushButton( "R", this );
    qArm_->setCheckable( true );
    qArm_->setFixedSize( 20, 20 );
    qArm_->setFont( btnFont );
    qArm_->setToolTip( "Arm for Recording\n(Right-click to select input channels)" );
    qArm_->setStyleSheet( "QPushButton:checked { background:#c04040; color:white; }" );
    qArm_->setContextMenuPolicy( Qt::CustomContextMenu );

    qTakes_ = new QPushButton( "T", this );
    qTakes_->setCheckable( true );
    qTakes_->setFixedSize( 20, 20 );
    qTakes_->setFont( btnFont );
    qTakes_->setToolTip( "Show take lanes (takes stack up when you record over a clip)" );
    qTakes_->setStyleSheet( "QPushButton:checked { background:#4080c0; color:white; }" );

    qGroup_ = new QPushButton( "G", this );
    qGroup_->setCheckable( true );
    qGroup_->setFixedSize( 20, 20 );
    qGroup_->setFont( btnFont );
    qGroup_->setToolTip( "Edit group: lock this track (and its subtree) together" );
    qGroup_->setStyleSheet( "QPushButton:checked { background:#40a060; color:white; }" );

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
    probe_.setTap( tk_.getRootComponent() );

    // Mute/Solo column, then the fader column, then the meter; a trailing
    // stretch keeps the group left-aligned.
    qStripRow_ = new QBoxLayout( QBoxLayout::LeftToRight );
    qStripRow_->setContentsMargins( 0, 0, 0, 0 );
    qStripRow_->setSpacing( 4 );
    qStripRow_->addLayout( qBtnCol_ );
    qStripRow_->addLayout( qFaderCol_ );
    qStripRow_->addWidget( qMeter_ );
    qStripRow_->addStretch( 1 );

    // The head is placed by the view at exactly its lane's size (which can be
    // small, and differs from lane to lane), so it must be free to take that
    // size: a minimum width only, and a layout that never pushes a minimum
    // height back onto the widget. updateLayout() hides what does not fit.
    setMinimumWidth( SMV_TRACK_CTRL_WIDTH );
    setMinimumHeight( 0 );
    setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Ignored );

    qLayout_->addWidget( qTrkLabel_, 0, 0, Qt::AlignTop );
    qLayout_->addLayout( qStripRow_, 1, 0 );
    qLayout_->setRowStretch( 1, 1 );
    qLayout_->setSizeConstraint( QLayout::SetNoConstraint );

    // Seed widgets from the current track state.
    setSliderSilently( tk_.getVolume() );
    qMute_->setChecked( tk_.isMuted() );
    qSolo_->setChecked( tk_.isSolo() );
    qArm_->setChecked( tk_.isArmedForRecording() );
    qTakes_->setChecked( smv_.isTrackTakesExpanded( &tk_ ) );
    qGroup_->setChecked( tk_.getEditGroup() != 0 );

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
    QObject::connect( &tk_, SIGNAL( editGroupChanged( int ) ),
                      this, SLOT( onEditGroupChanged( int ) ) );
    QObject::connect( &tk_, SIGNAL( mutedChanged( bool ) ),
                      this, SLOT( onMutedChanged( bool ) ) );
    QObject::connect( &tk_, SIGNAL( soloChanged( bool ) ),
                      this, SLOT( onSoloChanged( bool ) ) );
    QObject::connect( &tk_, SIGNAL( armedForRecordingChanged( bool ) ),
                      this, SLOT( onArmedChanged( bool ) ) );

    // Connect to mixer's track selection changes for highlighting
    SStdMixer *mixer = smv_.getModel();
    if( mixer ) {
        QObject::connect( mixer, SIGNAL( selectedTrackChanged( STrack * ) ),
                          this, SLOT( onSelectedTrackChanged( STrack * ) ) );
        // Check if this track is currently selected
        onSelectedTrackChanged( mixer->getSelectedTrack() );
    }

    // Seed the density from the size we start at, in case the first geometry
    // the view hands us happens to match the default one (no resize event).
    updateLayout();
}

void SSMVMixerControl::showChannelMenu()
{
    // Get the selected input device from settings
    SProject *project = SApplication::app().getCurrentProject();
    if( !project ) {
        qWarning( "No project available" );
        return;
    }

    QString inDeviceId = SSettings::instance().audioInputDeviceId();

    // Create a temporary audio input to query channel count
    std::unique_ptr<audio::AudioInput> input = audio::createAudioInput();
    if( !input || input->openDevice( inDeviceId.toStdString(), 48000 ) != 0 ) {
        qWarning( "Failed to open input device for channel enumeration" );
        return;
    }

    const audio::AudioInputConfig &config = input->getConfig();
    uint32_t numChannels = config.channels;
    if( numChannels == 0 ) {
        qWarning( "Input device has no channels" );
        return;
    }

    QMenu menu( "Input Channels" );
    uint32_t currentSelection = tk_.getRecordingChannels();

    // "All Channels" option
    QAction *allChannelsAction = menu.addAction( "All Channels" );
    allChannelsAction->setCheckable( true );
    allChannelsAction->setChecked( currentSelection == 0 );
    connect( allChannelsAction, &QAction::triggered, this, [this]() {
        setRecordingChannels( 0 );
    });

    menu.addSeparator();

    // Individual channels
    for( uint32_t ch = 0; ch < numChannels; ++ch ) {
        QString label = QString( "Channel %1" ).arg( ch + 1 );
        QAction *chAction = menu.addAction( label );
        chAction->setCheckable( true );
        chAction->setChecked( (currentSelection & (1U << ch)) != 0 );
        connect( chAction, &QAction::triggered, this, [this, ch]() {
            uint32_t channels = tk_.getRecordingChannels();
            if( channels == 0 ) {
                // "All" mode: switch to single channel
                channels = (1U << ch);
            } else {
                // Toggle this channel
                channels ^= (1U << ch);
            }
            setRecordingChannels( channels );
        });
    }

    menu.addSeparator();

    // Stereo pairs (1-2, 3-4, 5-6, etc.)
    for( uint32_t pair = 0; pair + 1 < numChannels; pair += 2 ) {
        QString label = QString( "Stereo %1-%2" ).arg( pair + 1 ).arg( pair + 2 );
        QAction *pairAction = menu.addAction( label );
        pairAction->setCheckable( true );
        uint32_t pairMask = (1U << pair) | (1U << (pair + 1));
        pairAction->setChecked( currentSelection == pairMask );
        connect( pairAction, &QAction::triggered, this, [this, pairMask]() {
            setRecordingChannels( pairMask );
        });
    }

    // Show menu at cursor position
    menu.exec( QCursor::pos() );
}

void SSMVMixerControl::setRecordingChannels( uint32_t channels )
{
    tk_.setRecordingChannels( channels );

    // Update the ARM button tooltip to show selected channels
    QString tooltip = "Arm for Recording\n(Right-click to select input channels)";
    if( channels != 0 ) {
        QString channelStr;
        for( uint32_t ch = 0; ch < 32; ++ch ) {
            if( channels & (1U << ch) ) {
                if( !channelStr.isEmpty() ) channelStr += ", ";
                channelStr += QString::number( ch + 1 );
            }
        }
        tooltip += QString( "\nSelected: %1" ).arg( channelStr );
    }
    qArm_->setToolTip( tooltip );
}

void SSMVMixerControl::onSelectedTrackChanged( STrack *track )
{
    bool wasSelected = selected_;
    selected_ = (track == &tk_);
    if( wasSelected != selected_ ) {
        update();  // Repaint to update background color
    }
}

// Proposal 34 — one metering tick for this track's meter.
void SSMVMixerControl::onMeterTick( offset_t pos, qint64 nowMs, bool live )
{
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

    bool audible = !tk_.isMuted();
    if( audible ) {
        SStdMixer *mixer = smv_.getModel();
        if( mixer && mixer->anyTrackSoloed() && !tk_.isSolo() ) audible = false;
    }
    if( !audible ) { qMeter_->pushIdle( nowMs ); return; }

    // Re-bind every tick rather than only in the ctor: setTap() is a pointer
    // compare that no-ops when unchanged, and it makes the meter self-healing if
    // the track's components are ever rebuilt under it — a meter silently dead
    // because it bound before its tap existed is a whole bug class avoided for
    // one comparison.
    probe_.setTap( tk_.getRootComponent() );

    twLevelSample s;
    if( probe_.advanceTo( pos, s ) ) qMeter_->pushLevel( s, nowMs );
    else                             qMeter_->pushIdle( nowMs );
}

QString SSMVMixerControl::describeMeter()
{
    if( !qMeter_ ) return QStringLiteral( "vis=0" );
    // Apply the density rules for the CURRENT size before describing them: Qt
    // does not deliver a resizeEvent to a widget that was never shown, so a
    // headless caller's resize() alone would describe the previous layout.
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
    for( QPushButton *b : { qMute_, qSolo_, qArm_, qTakes_, qGroup_ } )
        b->setFixedSize( btn, btn );

    switch( d ) {
    case Density::Full:
        // Buttons in a column beside a tall fader with its dB readout.
        qBtnCol_->setDirection( QBoxLayout::TopToBottom );
        qStripRow_->setDirection( QBoxLayout::LeftToRight );
        qMute_->show(); qSolo_->show();
        qArm_->show(); qTakes_->show(); qGroup_->show();
        qVolume_->show();
        qVolLabel_->show();
        qMeter_->show();
        break;
    case Density::Compact:
        // One row of small buttons above a horizontal fader; the dB readout
        // only survives while there is room for a second row.
        qBtnCol_->setDirection( QBoxLayout::LeftToRight );
        qStripRow_->setDirection( QBoxLayout::TopToBottom );
        qMute_->show(); qSolo_->show();
        qArm_->show(); qTakes_->show(); qGroup_->show();
        qVolume_->show();
        qVolLabel_->setVisible( height() >= 84 );
        // The meter is the first thing to go when the rows stack up: a fader you
        // cannot see is worse than a meter you cannot see.
        qMeter_->setVisible( height() >= 60 );
        break;
    case Density::Tiny:
        // Barely a lane: the name, and Mute/Solo beside it while a single
        // button row still fits under it. Hiding beats clipping — a
        // half-drawn fader reads as a rendering bug.
        qBtnCol_->setDirection( QBoxLayout::LeftToRight );
        qStripRow_->setDirection( QBoxLayout::TopToBottom );
        qArm_->hide(); qTakes_->hide(); qGroup_->hide();
        qMute_->setVisible( height() >= 38 );
        qSolo_->setVisible( height() >= 38 );
        qVolume_->hide();
        qVolLabel_->hide();
        qMeter_->hide();
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

void SSMVMixerControl::updateLayout()
{
    // Applied on every resize, not only on a mode flip: some rules (the dB
    // readout) depend on the height *within* a density. Every setter below
    // no-ops when the value is unchanged, so this stays cheap during a drag.
    wideMode_ = width() > WIDE_MODE_THRESHOLD;
    density_  = densityFor( height() );
    applyDensity( density_ );
}
