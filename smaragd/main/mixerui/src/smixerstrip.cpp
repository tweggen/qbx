#include "app/mixerui/smixerstrip.h"

#include <QCheckBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QUndoStack>
#include <QVBoxLayout>

#include "app/actions/sactionhistory.h"
#include "app/model/sclipcolors.h"
#include "app/model/sdefaultreset.h"
#include "app/model/sproject.h"
#include "app/model/ssolorules.h"
#include "app/objects/mixer/slaneorder.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/mixer/strackbroadcast.h"
#include "app/objects/track/sliveinputactions.h"
#include "app/objects/track/ssettrackmuteaction.h"
#include "app/objects/track/ssettracksoloaction.h"
#include "app/objects/track/ssettrackvolumeaction.h"
#include "app/objects/track/strack.h"
#include "app/objects/track/strackpath.h"
#include "app/pluginui/splugineffectstrip.h"
#include "app/shell/sapplication.h"
#include "app/shell/sautomationrecorder.h"
#include "app/shell/slivemonitor.h"
#include "app/timeline/sfadercurve.h"
#include "app/timeline/slevelmeter.h"
#include "app/timeline/ssendstrip.h"
#include "app/timeline/ssubmit.h"

namespace {

/// The M/S/R squares. 20 px is the arranger head's own Full-density size, so
/// the two mounts agree at a glance; a mixer strip never shrinks them, because
/// unlike a lane its width is the user's own choice (D9).
constexpr int BTN = 20;

}  // namespace

SMixerStrip::SMixerStrip( SStdMixer *mixer, STrack *track, QWidget *parent )
    : QWidget( parent ), mixer_( mixer ), track_( track )
{
    buildUi_();

    if( STrack *t = track_.data() ) {
        connect( t, &STrack::volumeChanged, this, &SMixerStrip::onTrackVolumeChanged );
        connect( t, &STrack::mutedChanged,  this, &SMixerStrip::onTrackMutedChanged );
        connect( t, &STrack::soloChanged,   this, &SMixerStrip::onTrackSoloChanged );
        connect( t, &STrack::armedForRecordingChanged,  this, &SMixerStrip::onTrackArmedChanged );
    }

    // The app BROADCASTS; every mount connects itself. A registry would have
    // to be poked from everywhere a strip is created or destroyed, and these
    // connections drop themselves when the strip does.
    connect( &SApplication::app(), &SApplication::meterTick,
             this, &SMixerStrip::onMeterTick );
    if( meter_ )
        connect( &SApplication::app(), &SApplication::meterReset,
                 meter_, &SLevelMeter::resetMeter );
}

SMixerStrip::~SMixerStrip() = default;

void SMixerStrip::buildUi_()
{
    STrack *t = track_.data();

    // NO EXPLICIT MINIMUM HEIGHT ANYWHERE IN THIS WIDGET (CONTRACT inv. 6).
    // `qSmartMinSize()` REPLACES a layout-derived minimum with an explicit one
    // rather than taking the larger, and a QBoxLayout handed less than its
    // minimum distributes the shortfall and lets its children overlap. The
    // width IS pinned, because a mixer strip's width is a chosen column and
    // the pane scrolls horizontally past it.
    setFixedWidth( WIDE_WIDTH );

    QVBoxLayout *outer = new QVBoxLayout( this );
    outer->setContentsMargins( 2, 2, 2, 2 );
    outer->setSpacing( 2 );

    // --- the SCROLLED half -------------------------------------------------
    scroll_ = new QScrollArea( this );
    scroll_->setObjectName( QStringLiteral( "mixerStripScroll" ) );
    scroll_->setWidgetResizable( true );
    scroll_->setFrameShape( QFrame::NoFrame );
    scroll_->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );

    scrollBody_ = new QWidget( scroll_ );
    QVBoxLayout *body = new QVBoxLayout( scrollBody_ );
    body->setContentsMargins( 0, 0, 0, 0 );
    body->setSpacing( 2 );

    QHBoxLayout *header = new QHBoxLayout();
    header->setContentsMargins( 0, 0, 0, 0 );
    header->setSpacing( 2 );
    nameLabel_ = new QLabel( t ? t->getSName() : QString(), scrollBody_ );
    nameLabel_->setToolTip( nameLabel_->text() );
    header->addWidget( nameLabel_, 1 );
    narrowBtn_ = new QPushButton( QStringLiteral( "<" ), scrollBody_ );
    narrowBtn_->setFixedSize( 16, 16 );
    narrowBtn_->setToolTip( tr( "Narrow / wide strip" ) );
    connect( narrowBtn_, &QPushButton::clicked,
             this, [this]{ setNarrow( !narrow_ ); } );
    header->addWidget( narrowBtn_, 0 );
    body->addLayout( header );

    // THE ARRANGER'S OWN WIDGETS, MOUNTED. Not a compact re-implementation:
    // the editor registry, the drop handling and the Missing / Unsupported
    // states would each have to be written a second time and could then
    // disagree with the Track Detail dock's.
    inserts_ = new SPluginEffectStrip( t, scrollBody_ );
    body->addWidget( inserts_, 0 );
    sends_ = new SSendStrip( t, scrollBody_ );
    body->addWidget( sends_, 0 );
    body->addStretch( 1 );

    scroll_->setWidget( scrollBody_ );
    outer->addWidget( scroll_, 1 );

    // --- the PINNED half (D5) ----------------------------------------------
    fixedBlock_ = new QWidget( this );
    QVBoxLayout *fixed = new QVBoxLayout( fixedBlock_ );
    fixed->setContentsMargins( 0, 0, 0, 0 );
    fixed->setSpacing( 2 );

    QHBoxLayout *msr = new QHBoxLayout();
    msr->setContentsMargins( 0, 0, 0, 0 );
    msr->setSpacing( 2 );
    auto mkBtn = [&]( const QString &glyph, const QString &tip ) {
        QPushButton *b = new QPushButton( glyph, fixedBlock_ );
        b->setCheckable( true );
        b->setFixedSize( BTN, BTN );
        b->setToolTip( tip );
        msr->addWidget( b );
        return b;
    };
    muteBtn_ = mkBtn( QStringLiteral( "M" ), tr( "Mute" ) );
    soloBtn_ = mkBtn( QStringLiteral( "S" ), tr( "Solo" ) );
    armBtn_  = mkBtn( QStringLiteral( "R" ), tr( "Arm for recording" ) );
    msr->addStretch( 1 );
    fixed->addLayout( msr );
    connect( muteBtn_, &QPushButton::toggled, this, &SMixerStrip::onMuteToggled );
    connect( soloBtn_, &QPushButton::toggled, this, &SMixerStrip::onSoloToggled );
    connect( armBtn_,  &QPushButton::toggled, this, &SMixerStrip::onArmToggled );

    QHBoxLayout *faderRow = new QHBoxLayout();
    faderRow->setContentsMargins( 0, 0, 0, 0 );
    faderRow->setSpacing( 2 );

    fader_ = new QSlider( Qt::Vertical, fixedBlock_ );
    fader_->setRange( SFADER_MIN, SFADER_MAX );
    fader_->setSingleStep( 10 );     // 1.0 dB, as the arranger head's
    fader_->setPageStep( 60 );       // 6.0 dB
    fader_->setTickPosition( QSlider::TicksRight );
    fader_->setTickInterval( 120 );
    connect( fader_, &QSlider::valueChanged, this, &SMixerStrip::onFaderMoved );
    connect( fader_, &QSlider::sliderReleased, this, &SMixerStrip::onFaderReleased );
    // Double-click resets to EXACTLY 0.0 dB, committed as a dB rather than
    // through the slider: the curve does not round-trip (`sDbToFader( 0.0 )`
    // is tick -191 and back is +0.0625 dB).
    sdefaultreset::onDoubleClick( fader_, [this]{ applyVolumeDb_( 0.0 ); } );
    faderRow->addWidget( fader_, 0 );

    meter_ = new SLevelMeter( fixedBlock_ );
    meter_->setOrientation( Qt::Vertical );
    meter_->setMeterLabel( QString() );
    faderRow->addWidget( meter_, 0 );
    faderRow->addStretch( 1 );
    fixed->addLayout( faderRow, 1 );

    dbLabel_ = new QLabel( QStringLiteral( "+0.0 dB" ), fixedBlock_ );
    fixed->addWidget( dbLabel_, 0 );

    outer->addWidget( fixedBlock_, 0 );

    if( t ) {
        probe_.setTap( t->getRootComponent() );
        syncMeterLanes_();
        QSignalBlocker bm( muteBtn_ ), bs( soloBtn_ ), ba( armBtn_ );
        muteBtn_->setChecked( t->isMuted() );
        soloBtn_->setChecked( t->isSolo() );
        armBtn_->setChecked( t->isArmedForRecording() );
        setFaderSilently_( t->getVolume() );
    }
}

// --- the fader --------------------------------------------------------------

void SMixerStrip::onFaderMoved( int value )
{
    if( updating_ ) return;
    applyVolumeDb_( sFaderToDb( value ) );
}

void SMixerStrip::onFaderReleased()
{
    SApplication::app().automationRecorder().releaseControl();
}

// THE RECORDER IS OFFERED THE VALUE FIRST, AND A TAKEN VALUE SUBMITS NOTHING
// (proposal 37 P6). Skip this and a Touch/Latch/Write pass driven from the
// mixer puts one undo entry on the stack per slider tick -- thirty a second,
// which is precisely what P6 exists to prevent. The arranger's fader and the
// plugin parameter slider already feed the same pass; this is the third
// control, and the obligation is UI-side because the redirect that turns a
// volume write into an automation POINT lives inside the ACTION, and the
// offer does not.
void SMixerStrip::applyVolumeDb_( double db )
{
    STrack   *t     = track_.data();
    SStdMixer *mixer = mixer_.data();
    if( !t ) return;

    const QList<int> path =
        mixer ? strackpath::pathOf( mixer, t ) : QList<int>();
    if( path.isEmpty() ) {
        t->setVolume( db );
        if( SProject *p = SApplication::app().getCurrentProject() )
            p->notifyArrangementChanged();
        return;
    }

    SAutomationRecorder::Target target;
    target.ownerPath = path;
    target.target    = QStringLiteral( "self:Volume" );
    if( SApplication::app().isPlaying()
        && SApplication::app().automationRecorder().writeTick(
               target, db, SApplication::app().getGlobalLocatorPos() ) )
        return;

    stimeline::submitActive( new SSetTrackVolumeAction( path, db ) );
}

void SMixerStrip::setFaderSilently_( double db )
{
    if( !fader_ ) return;
    updating_ = true;
    { QSignalBlocker b( fader_ ); fader_->setValue( sDbToFader( db ) ); }
    if( dbLabel_ )
        dbLabel_->setText( QStringLiteral( "%1 dB" )
                               .arg( db, 0, 'f', 1, QLatin1Char( '+' ) ) );
    updating_ = false;
}

void SMixerStrip::onTrackVolumeChanged( double db ) { setFaderSilently_( db ); }

// --- M / S / R --------------------------------------------------------------

// D3's rule lives in `strackbroadcast::targetsFor`; the MACRO is three lines
// at each mount and is duplicated ON PURPOSE (`objects/mixer` can reach
// neither `stimeline::submitActive` nor the undo stack, and widening
// `SAppContext` for one convenience is the wrong trade). What may never be
// duplicated is the target COMPUTATION, and it is not.
void SMixerStrip::broadcast_( const char *macroLabel,
                              const std::function<bool( STrack * )> &needsChange,
                              const std::function<void( STrack * )> &submitOne )
{
    STrack    *t     = track_.data();
    SStdMixer *mixer = mixer_.data();
    if( !t || !mixer ) return;

    const QList<STrack *> targets = strackbroadcast::targetsFor( mixer, t );
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    const bool macro = targets.size() > 1 && stack;
    if( macro ) stack->beginMacro( QString::fromLatin1( macroLabel ) );
    for( STrack *target : targets ) {
        if( !needsChange( target ) ) continue;   // nothing to undo for this one
        submitOne( target );
    }
    if( macro ) stack->endMacro();
}

void SMixerStrip::onMuteToggled( bool on )
{
    if( updating_ ) return;
    SStdMixer *mixer = mixer_.data();
    broadcast_( "Mute tracks",
                [on]( STrack *t ) { return t->isMuted() != on; },
                [on, mixer]( STrack *t ) {
                    stimeline::submitActive( new SSetTrackMuteAction(
                        strackpath::pathOf( mixer, t ), on ) );
                } );
}

void SMixerStrip::onSoloToggled( bool on )
{
    if( updating_ ) return;
    SStdMixer *mixer = mixer_.data();
    broadcast_( "Solo tracks",
                [on]( STrack *t ) { return t->isSolo() != on; },
                [on, mixer]( STrack *t ) {
                    stimeline::submitActive( new SSetTrackSoloAction(
                        strackpath::pathOf( mixer, t ), on ) );
                } );
}

void SMixerStrip::onArmToggled( bool on )
{
    if( updating_ ) return;
    SStdMixer *mixer = mixer_.data();
    broadcast_( "Arm tracks",
                [on]( STrack *t ) { return t->isArmedForRecording() != on; },
                [on, mixer]( STrack *t ) {
                    stimeline::submitActive( new SArmTrackAction(
                        strackpath::pathOf( mixer, t ), on ) );
                } );
}

void SMixerStrip::onTrackMutedChanged( bool on )
{
    if( !muteBtn_ ) return;
    QSignalBlocker b( muteBtn_ );
    muteBtn_->setChecked( on );
}

void SMixerStrip::onTrackSoloChanged( bool on )
{
    if( !soloBtn_ ) return;
    QSignalBlocker b( soloBtn_ );
    soloBtn_->setChecked( on );
}

void SMixerStrip::onTrackArmedChanged( bool on )
{
    if( !armBtn_ ) return;
    QSignalBlocker b( armBtn_ );
    armBtn_->setChecked( on );
}

// --- the meter --------------------------------------------------------------

int SMixerStrip::syncMeterLanes_()
{
    STrack *t = track_.data();
    if( !meter_ || !t ) return 1;
    // THE WIDTH COMES FROM THE TAP, never from SProject: asking the component
    // keeps the meter and the audio reading ONE number, and it is the same
    // number the probe's width check compares a cached page against.
    std::shared_ptr<twComponent> tap = t->getRootComponent();
    const int width = tap ? (int) tap->getOutputChannels() : 1;
    // TWO LANES, as the arranger head and the transport master meter show.
    // A 60 px strip has no room for six, and the pair you can actually hear
    // is what the device rule delivers. The cap is ANNOUNCED, never silent:
    // describe() reports lanes and width separately, and the tooltip names
    // the width and points at the Track Detail dock.
    const int shown = qMin( width > 0 ? width : 1, SLevelMeter::MONITOR_LANES );
    meter_->setLanes( shown, width );
    if( width > SLevelMeter::MONITOR_LANES )
        meter_->setToolTip( tr( "Showing %1 of %2 channels - the Track Detail "
                                "dock shows every channel" )
                                .arg( shown ).arg( width ) );
    return shown;
}

void SMixerStrip::onMeterTick( offset_t pos, qint64 nowMs, bool live )
{
    // The READ-value pump runs FIRST and unconditionally: a Read-family lane
    // must move the fader even on a strip whose meter is hidden.
    pumpReadValue_( pos );

    if( !meter_ || !meter_->isVisible() ) return;
    STrack    *t     = track_.data();
    SStdMixer *mixer = mixer_.data();
    if( !t ) return;

    if( !live ) { meter_->pushIdle( nowMs ); return; }

    // AUDIBILITY IS ASKED, NEVER RE-DERIVED. main/timeline/CONTRACT.md
    // inv. 10 records that two local copies of the direct-children-only rule
    // are exactly how the meter and the ear came to disagree about a nested
    // lane. A solo-muted lane IDLES; it never gets a special reading.
    if( !ssolo::isLaneAudible( mixer, t ) ) { meter_->pushIdle( nowMs ); return; }

    // A LIVE-OWNED LANE HAS NO FROZEN PAGES TO READ, so the meter shows the
    // pre-FX INPUT level instead - the arranger head's own branch. This is a
    // SECOND term beside audibility and is never folded into it: a live-owned
    // track is still audible in every other sense (proposal 21 L1b).
    if( t->isLiveOwnedLane() ) {
        if( SLiveMonitor *mon = SApplication::app().liveMonitor() ) {
            // PEEK, NEVER TAKE. takeInputPeak() CLEARS the source's peak, so
            // with the arranger head and this strip both ticking, whichever
            // ran second would read 0 and its meter would sit dead. That
            // hazard did not exist while the head was the only mount, which
            // is exactly the class of defect this proposal's governing rule
            // is about. The head keeps taking (and so keeps clearing); a
            // second mount may only look.
            const double peak = mon->peekInputPeak( t );
            twLevelSample ls;
            ls.peak       = (float) peak;
            ls.meanSquare = (float) ( peak * peak );  // no RMS window on the pump
            ls.frames     = 1;
            ls.clipped    = peak >= 1.0;
            meter_->setMeterLabel(
                tr( "Input level (pre-FX) - this track is MONITORED, so it "
                    "has no frozen pages to read" ) );
            meter_->pushLevel( ls, nowMs );
            return;
        }
    }

    probe_.setTap( t->getRootComponent() );
    const int shown = syncMeterLanes_();
    twLevelSampleSet s;
    // A PAGE MISS DECAYS THE METER, never holds it: a dropout then reads as a
    // fast fall rather than as a frozen bar. Nothing here may block, wait or
    // create a demand.
    if( probe_.advanceTo( pos, s, shown ) ) meter_->pushLevel( s, nowMs );
    else                                    meter_->pushIdle( nowMs );
}

void SMixerStrip::pumpReadValue_( offset_t pos )
{
    STrack    *t     = track_.data();
    SStdMixer *mixer = mixer_.data();
    if( !t || !fader_ ) return;

    SAutomationLane *lane = t->automationLane( QStringLiteral( "self:Volume" ) );
    if( !lane || !SAutomationRecorder::isReadFamily( lane->mode() ) ) {
        lastReadDb_ = 1e30;
        return;
    }
    SAutomationRecorder::Target target;
    target.ownerPath = mixer ? strackpath::pathOf( mixer, t ) : QList<int>();
    target.target    = QStringLiteral( "self:Volume" );
    // A control being RECORDED shows the HAND, not the curve.
    if( SApplication::app().automationRecorder().isRecording( target ) ) return;

    const double db = lane->valueAt( pos );
    if( qAbs( db - lastReadDb_ ) < 0.05 ) return;
    lastReadDb_ = db;
    setFaderSilently_( db );
}

// --- sections, narrow, describe, detach -------------------------------------

void SMixerStrip::setNarrow( bool narrow )
{
    if( narrow_ == narrow ) return;
    narrow_ = narrow;
    setFixedWidth( narrow_ ? NARROW_WIDTH : WIDE_WIDTH );
    if( narrowBtn_ )
        narrowBtn_->setText( narrow_ ? QStringLiteral( ">" )
                                     : QStringLiteral( "<" ) );
    setSectionsVisible( showInserts_, showSends_, showMeter_, showFader_ );
}

void SMixerStrip::setSectionsVisible( bool inserts, bool sends,
                                      bool meter, bool fader )
{
    showInserts_ = inserts;
    showSends_   = sends;
    showMeter_   = meter;
    showFader_   = fader;
    // NARROW KEEPS M/S/R, THE METER AND THE FADER and drops the rest (D9).
    // It is not a density ladder: a lane's HEIGHT is imposed on it by the
    // arrangement, whereas a strip's width is chosen, so this is a two-state
    // user choice rather than a graceful degradation.
    if( inserts_ ) inserts_->setVisible( showInserts_ && !narrow_ );
    if( sends_ )   sends_->setVisible( showSends_ && !narrow_ );
    if( meter_ )   meter_->setVisible( showMeter_ );
    if( fader_ )   fader_->setVisible( showFader_ );
    if( dbLabel_ ) dbLabel_->setVisible( showFader_ );
    if( nameLabel_ && track_ ) {
        const QString full = track_->getSName();
        nameLabel_->setText(
            narrow_ ? nameLabel_->fontMetrics().elidedText(
                          full, Qt::ElideRight, NARROW_WIDTH - 24 )
                    : full );
        nameLabel_->setToolTip( full );
    }
}

QString SMixerStrip::describe() const
{
    STrack *t = track_.data();
    const double db = t ? t->getVolume() : 0.0;
    return QStringLiteral(
               "name=%1|narrow=%2|mute=%3|solo=%4|arm=%5|db=%6|role=%7"
               "|inserts=%8|sends=%9|meter=%10" )
        .arg( t ? t->getSName() : QStringLiteral( "?" ) )
        .arg( narrow_ ? 1 : 0 )
        .arg( t && t->isMuted() ? 1 : 0 )
        .arg( t && t->isSolo() ? 1 : 0 )
        .arg( t && t->isArmedForRecording() ? 1 : 0 )
        .arg( db, 0, 'f', 1 )
        .arg( QString::fromLatin1(
            systemRoleToString( t ? t->systemRole() : SSystemRole::None ) ) )
        .arg( inserts_ && !inserts_->isHidden() ? 1 : 0 )
        .arg( sends_ && !sends_->isHidden() ? 1 : 0 )
        .arg( meter_ ? meter_->describe() : QString() );
}

// CONTRACT inv. 4 / D13. closeProject() clears the undo stack, calls
// destroyDocksToolbars() and THEN deletes the project; a strip still holding
// an STrack* and a bound twLevelProbe dereferences freed memory on the next
// 33 ms tick. That is a crash, not a glitch, and it is the same class as the
// SCut revalidation UAF.
void SMixerStrip::detachProject()
{
    if( track_ ) track_->disconnect( this );
    track_ = nullptr;
    mixer_ = nullptr;
    probe_.setTap( nullptr );
    if( meter_ ) meter_->resetMeter();
}
