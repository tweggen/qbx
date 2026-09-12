#include "app/mixerui/smixerstrip.h"

#include <QCheckBox>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QMenu>
#include <QSlider>
#include <QUndoStack>
#include <QWheelEvent>
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
#include "app/actions/saction.h"
#include "app/shell/sapplication.h"
#include "app/shell/sautomationrecorder.h"
#include "app/shell/slivemonitor.h"
#include "app/timeline/sfadercurve.h"
#include "app/timeline/slevelmeter.h"
#include "app/timeline/ssendstrip.h"
#include "app/timeline/strackgestures.h"
#include "tw/core/twlog.h"

namespace {

/// The M/S/R squares. 20 px is the arranger head's own Full-density size, so
/// the two mounts agree at a glance in the wide strip.
///
/// A NARROW strip shrinks them, and that is arithmetic rather than taste:
/// three 20 px squares plus two 2 px gaps plus the layout's 4 px of margins
/// need 68 px, which does not fit the 60 px D9 names -- measured as
/// `SMixerStrip(w 60<72)`, i.e. the strip's own layout minimum exceeding the
/// width it was pinned to, with two overlapping pairs behind it. At 16 px the
/// row needs 56 and fits. This is NOT the head's density ladder: that exists
/// because a lane's HEIGHT is imposed by the arrangement, whereas a strip's
/// width is the user's own two-state choice.
constexpr int BTN        = 20;
/// The header label's horizontal padding, in the stylesheet AND in the
/// elision budget. One constant, because the two must agree exactly.
constexpr int NAME_PAD_PX = 2;
constexpr int BTN_NARROW = 16;

}  // namespace

SMixerStrip::SMixerStrip( SStdMixer *mixer, STrack *track,
                          const QString &rootName, QWidget *parent )
    : QWidget( parent ), mixer_( mixer ), track_( track ), rootName_( rootName )
{
    buildUi_();

    if( STrack *t = track_.data() ) {
        connect( t, &STrack::volumeChanged, this, &SMixerStrip::onTrackVolumeChanged );
        connect( t, &STrack::mutedChanged,  this, &SMixerStrip::onTrackMutedChanged );
        connect( t, &STrack::soloChanged,   this, &SMixerStrip::onTrackSoloChanged );
        connect( t, &STrack::armedForRecordingChanged,  this, &SMixerStrip::onTrackArmedChanged );
    }

    // THE STRIP DOES NOT CONNECT TO `meterTick` ITSELF (proposal 48 M3). The
    // PANE takes the broadcast once and dispatches, because the dock gate has
    // to be answered BEFORE the per-strip walk: `mixerui/CONTRACT.md` inv. 5
    // says a hidden dock does no work "not even the model walk", and a gate
    // sitting inside each strip has already paid for the walk by the time it
    // runs. One connection instead of N is the smaller consequence.
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
    // (the header is added to `outer` above this, so it sits on top)
    scroll_ = new QScrollArea( this );
    scroll_->setObjectName( QStringLiteral( "mixerStripScroll" ) );
    scroll_->setWidgetResizable( true );
    scroll_->setFrameShape( QFrame::NoFrame );
    scroll_->setHorizontalScrollBarPolicy( Qt::ScrollBarAlwaysOff );
    // See setSectionsVisible(): a QScrollArea's own minimum size hint is a
    // Qt floor far wider than a mixer column, and qSmartMinSize() bounds a
    // minimum by the maximum. Applied HERE as well because setNarrow() early-
    // returns when the flag is unchanged, so a strip that is born wide would
    // otherwise never get it.
    scroll_->setMaximumWidth( WIDE_WIDTH );

    scrollBody_ = new QWidget( scroll_ );
    QVBoxLayout *body = new QVBoxLayout( scrollBody_ );
    body->setContentsMargins( 0, 0, 0, 0 );
    body->setSpacing( 2 );

    // THE NAME HEADER IS OUTSIDE THE SCROLL AREA, and this is a deliberate
    // departure from D5's diagram, which drew it inside. Two reasons, one of
    // them measured:
    //
    //  - a strip's NAME is its identity rather than its content, and D5's own
    //    rule for the fader block -- "what a user looks at while the transport
    //    runs stays put" -- applies to knowing WHICH track they are looking at
    //    at least as strongly;
    //  - a QAbstractScrollArea carries a minimum size hint of its own, several
    //    times the font height, whatever it contains. With the header inside
    //    it, a NARROW strip demanded 70 px against the 60 D9 names even with
    //    the inserts and the sends hidden and the scrollbar off. Outside it,
    //    the scroll area can be hidden outright in narrow mode -- where it has
    //    nothing to show anyway -- and the 60 px strip fits.
    QHBoxLayout *header = new QHBoxLayout();
    header->setContentsMargins( 0, 0, 0, 0 );
    header->setSpacing( 2 );
    nameLabel_ = new QLabel( t ? t->getSName() : QString(), this );
    nameLabel_->setToolTip( nameLabel_->text() );
    header->addWidget( nameLabel_, 1 );
    narrowBtn_ = new QPushButton( QStringLiteral( "<" ), this );
    narrowBtn_->setFixedSize( 16, 16 );
    narrowBtn_->setToolTip( tr( "Narrow / wide strip" ) );
    connect( narrowBtn_, &QPushButton::clicked,
             this, [this]{ setNarrow( !narrow_ ); } );
    header->addWidget( narrowBtn_, 0 );
    outer->addLayout( header );

    // THE ARRANGER'S OWN WIDGETS, MOUNTED. Not a compact re-implementation:
    // the editor registry, the drop handling and the Missing / Unsupported
    // states would each have to be written a second time and could then
    // disagree with the Track Detail dock's.
    inserts_ = new SPluginEffectStrip( t, scrollBody_ );
    // COMPACT ALWAYS, in both widths (D5a). Even a WIDE mixer strip is 96 px
    // against an insert row that wants 113 for its two Add buttons alone --
    // this is a mixer column, not the Track Detail dock, and the detail dock
    // keeps the full row.
    inserts_->setCompact( true );
    body->addWidget( inserts_, 0 );
    sends_ = new SSendStrip( t, scrollBody_ );
    sends_->setCompact( true );
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
    msrLayout_ = msr;
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
    faderLayout_ = faderRow;
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
    // ONE WHEEL NOTCH IS ONE dB (AC4.4). See eventFilter(): the step is
    // `sFaderWheelValue()` in `sfadercurve.h`, shared with the arranger head,
    // because a fader is a fader in both mounts and the curve makes "1 dB" a
    // question a singleStep cannot answer.
    fader_->installEventFilter( this );
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
        narrow_ = t->mixerStripNarrow();
    }
    applyHeaderColor_();
}

// AC4.3. THE TRACK'S COLOUR REACHES THE MIXER THROUGH THE SAME FUNCTION THE
// ARRANGER PAINTS WITH, resolved from the PROJECT root -- which is what
// `strackrndrinline.cpp`, `sstdmixerview.cpp`'s take lanes and the shell's own
// `sClipBodyOf()` (the pixel gates' classifier) all ask. Not "the same colour
// scheme": the same call. A second palette here is the drift proposal 41 M7
// fixed for the tag chip and the clip palette fixed for the classifiers, and
// this proposal's governing rule forbids it outright.
//
// The header LABEL is tinted rather than the whole strip: a mixer column is
// mostly controls, and a saturated 96 px block behind a fader is unreadable.
// The text colour follows the anchor's own lightness so a light anchor does
// not get white-on-white -- derived, never a second palette entry.
QColor SMixerStrip::headerColor() const { return headerColor_; }

void SMixerStrip::applyHeaderColor_()
{
    STrack *t = track_.data();
    SProject *proj = SApplication::app().getCurrentProject();
    SObject *root  = proj ? proj->getRootComponent() : nullptr;
    const int idx = ( root && t ) ? sclipcolors::indexForLane( *root, *t ) : 0;
    headerColor_ = sclipcolors::body( idx, false, t && t->isMuted() );
    if( !nameLabel_ ) return;
    const QColor fg = headerColor_.lightness() > 140 ? QColor( 20, 20, 20 )
                                                     : QColor( 235, 235, 235 );
    nameLabel_->setAutoFillBackground( true );
    nameLabel_->setStyleSheet(
        QStringLiteral( "QLabel { background:%1; color:%2; padding:1px %3px; }" )
            .arg( headerColor_.name(), fg.name() )
            .arg( NAME_PAD_PX ) );
}

// PROPOSAL 48 D12 -- THE STRIP STAMPS ITS OWN ARRANGEMENT, never
// `stimeline::submitActive`. That helper asks which EDITOR TAB is active and
// stamps THAT root, which is right for a widget living inside one arranger
// and wrong for a pane that can be showing a different arrangement than the
// tab in front: the action would resolve its path against the wrong tree and
// SILENTLY DO NOTHING (an empty path resolves to no object). This repository
// has already shipped that shape once -- `SClearSelectionAction` honours
// `pathRoot_` and the convenience helper did not set one, so Ctrl+Shift+A
// cleared the master's selection from any tab.
void SMixerStrip::submit_( SAction *a ) const
{
    if( !a ) return;
    a->setPathRoot( rootName_ );
    SApplication::app().submitAction( a );
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

    submit_( new SSetTrackVolumeAction( path, db ) );
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
                [on, mixer, this]( STrack *t ) {
                    submit_( new SSetTrackMuteAction(
                        strackpath::pathOf( mixer, t ), on ) );
                } );
}

void SMixerStrip::onSoloToggled( bool on )
{
    if( updating_ ) return;
    SStdMixer *mixer = mixer_.data();
    broadcast_( "Solo tracks",
                [on]( STrack *t ) { return t->isSolo() != on; },
                [on, mixer, this]( STrack *t ) {
                    submit_( new SSetTrackSoloAction(
                        strackpath::pathOf( mixer, t ), on ) );
                } );
}

void SMixerStrip::onArmToggled( bool on )
{
    if( updating_ ) return;
    SStdMixer *mixer = mixer_.data();
    broadcast_( "Arm tracks",
                [on]( STrack *t ) { return t->isArmedForRecording() != on; },
                [on, mixer, this]( STrack *t ) {
                    submit_( new SArmTrackAction(
                        strackpath::pathOf( mixer, t ), on ) );
                } );
}

void SMixerStrip::onTrackMutedChanged( bool on )
{
    if( !muteBtn_ ) return;
    QSignalBlocker b( muteBtn_ );
    muteBtn_->setChecked( on );
    // `sclipcolors::body()` takes the muted flag, so the header follows the
    // mute exactly as a clip body does. Re-resolved rather than cached per
    // state: the resolve is one palette lookup once the index is known.
    applyHeaderColor_();
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

bool SMixerStrip::onMeterTick( offset_t pos, qint64 nowMs, bool live )
{
    // The READ-value pump runs FIRST and unconditionally: a Read-family lane
    // must move the fader even on a strip whose meter is hidden.
    pumpReadValue_( pos );

    // NOT a visibility test. A strip scrolled out of the viewport MUST still
    // tick (inv. 5: its meter is a few px of paint, and stopping it would
    // freeze a bar the user scrolls back to), and a clipped widget is still
    // `isVisible()` — so that test never did what it looked like it did. What
    // legitimately stops the work is the METER SECTION being switched off,
    // which is a user's explicit choice, and the DOCK being hidden, which the
    // pane answers before it walks at all.
    if( !meter_ || !showMeter_ ) return false;
    STrack    *t     = track_.data();
    SStdMixer *mixer = mixer_.data();
    if( !t ) return false;

    if( !live ) { meter_->pushIdle( nowMs ); return false; }

    // AUDIBILITY IS ASKED, NEVER RE-DERIVED. main/timeline/CONTRACT.md
    // inv. 10 records that two local copies of the direct-children-only rule
    // are exactly how the meter and the ear came to disagree about a nested
    // lane. A solo-muted lane IDLES; it never gets a special reading.
    if( !ssolo::isLaneAudible( mixer, t ) ) { meter_->pushIdle( nowMs ); return true; }

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
            return true;
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
    return true;
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

// NARROW IS THE TRACK'S OWN FLAG (proposal 48 M2), a fifth sibling of the four
// pieces of per-track view state proposal 46 M3 moved onto STrack. It
// therefore survives a save/load for free and needs no pruning walk -- see
// `STrack::mixerStripNarrow()` for why that deviates from D9, whose stated
// reason ("STrack attributes are serialized with the arrangement") excludes
// its four siblings equally, and whose prescribed walk was retired by 46 M3.
void SMixerStrip::setNarrow( bool narrow )
{
    if( narrow_ == narrow ) return;
    narrow_ = narrow;
    if( STrack *t = track_.data() ) t->setMixerStripNarrow( narrow );
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
    const int btn = narrow_ ? BTN_NARROW : BTN;
    for( QPushButton *b : { muteBtn_, soloBtn_, armBtn_ } )
        if( b ) b->setFixedSize( btn, btn );
    // The dB READOUT goes with the width: "+0.0 dB" needs about as much room
    // as the three buttons do, and the fader's own position still shows the
    // level. The number comes back the moment the strip is widened.
    if( dbLabel_ ) dbLabel_->setVisible( showFader_ && !narrow_ );
    // A NARROW strip has NOTHING SCROLLABLE -- the inserts and the sends are
    // both hidden -- so its scroll area's vertical scrollbar is pure cost, and
    // a QScrollArea reserves that extent in its own minimum width whether the
    // bar is ever shown or not. Measured: it was 10 of the 70 px the strip
    // demanded against the 60 D9 names.
    // A QScrollArea CARRIES A LARGE MINIMUM SIZE HINT OF ITS OWN, whatever it
    // contains -- measured at 113 px here, which is more than a WIDE mixer
    // strip is. That is a Qt floor and no amount of compacting the content
    // moves it. `qSmartMinSize()` bounds a widget's minimum BY ITS MAXIMUM,
    // so pinning the scroll area to the strip's own width is what lets a
    // column be a column. This is the legitimate use of an explicit maximum:
    // a scroll area's entire job is to be smaller than its contents, which is
    // the opposite of the case `main/timeline/CONTRACT.md` inv. 45 warns
    // about (a widget UNDER-reporting what it needs).
    const int w = narrow_ ? NARROW_WIDTH : WIDE_WIDTH;
    if( scroll_ ) {
        scroll_->setMaximumWidth( w );
        // Nothing in it is shown when narrow, so it is hidden outright.
        scroll_->setVisible( !narrow_ && ( showInserts_ || showSends_ ) );
    }
    const int pad = narrow_ ? 1 : 2;
    if( QLayout *l = layout() ) l->setContentsMargins( pad, pad, pad, pad );
    if( msrLayout_ ) msrLayout_->setSpacing( pad );
    if( faderLayout_ ) faderLayout_->setSpacing( pad );
    if( inserts_ ) inserts_->setVisible( showInserts_ && !narrow_ );
    if( sends_ )   sends_->setVisible( showSends_ && !narrow_ );
    if( meter_ )   meter_->setVisible( showMeter_ );
    if( fader_ )   fader_->setVisible( showFader_ );
    // THE NAME ELIDES IN BOTH WIDTHS, and getting this wrong is what made the
    // WIDE strip fail AC1.7. A QLabel's minimum width is its FULL TEXT --
    // measured at 91 px for an ordinary generated track name, which with the
    // narrow-toggle button and the margins is exactly the 113 px a 96 px
    // strip was reported as owing. A mixer strip is a fixed-width column and
    // a name that does not fit is truncated, as it is in every reference DAW;
    // the full name stays in the tooltip and in describe().
    if( nameLabel_ ) {
        const QString full = track_ ? track_->getSName() : QString();
        // Less the toggle button AND the header's own colour padding. That
        // padding is not decoration accounting: AC4.3 tints the label with a
        // stylesheet, and a QLabel's minimum width is its text PLUS its
        // padding -- so forgetting the 4 px here put the NARROW strip's own
        // layout minimum at 61 against the 60 D9 names, reported as
        // `SMixerStrip(w 60<61)` by three cases at once. The same class of
        // Qt floor M1 paid four times over.
        const int avail = qMax( 8, w - 16 - 3 * pad - 2 * NAME_PAD_PX );
        nameLabel_->setMaximumWidth( avail );
        nameLabel_->setText(
            nameLabel_->fontMetrics().elidedText( full, Qt::ElideRight, avail ) );
        nameLabel_->setToolTip( full );
    }
}

// A GESTURE SEAM, not a model write. It moves the REAL control and lets Qt
// deliver the signal, so a missing connect() fails the gate -- the property
// proposal 47 M5 established for the sends strip and the reason a widget can
// be gated where a context menu cannot.
bool SMixerStrip::driveControl( const QString &control, bool on )
{
    if( control == QLatin1String( "narrow" ) ) { setNarrow( on ); return true; }
    QPushButton *b = control == QLatin1String( "mute" ) ? muteBtn_
                   : control == QLatin1String( "solo" ) ? soloBtn_
                   : control == QLatin1String( "arm" )  ? armBtn_
                                                        : nullptr;
    if( !b ) return false;
    if( b->isChecked() == on ) return true;   // already there; nothing to click
    b->click();
    return true;
}

bool SMixerStrip::driveValue( const QString &control, const QString &gesture,
                              double value )
{
    if( control != QLatin1String( "fader" ) || !fader_ ) return false;

    if( gesture == QLatin1String( "double-click" ) ) {
        // The RESET route. It commits a dB DIRECTLY rather than moving the
        // slider, because the integer fader's curve does not round-trip
        // (`sDbToFader( 0.0 )` is tick -191 and back is +0.0625 dB) -- and it
        // goes through applyVolumeDb_, so during an open pass the recorder
        // takes it exactly as an ordinary drag tick would (AC3a.3).
        applyVolumeDb_( 0.0 );
        return true;
    }

    if( gesture == QLatin1String( "wheel" ) ) {
        // A REAL WHEEL EVENT at the fader, so what runs is `eventFilter`'s own
        // branch rather than a second copy of its arithmetic. `value` is
        // NOTCHES; Qt's unit is 1/8 degree and one notch is 15 degrees, hence
        // 120 per notch -- the same number the filter divides by.
        const int notches = (int) value;
        const QPointF c( fader_->width() / 2.0, fader_->height() / 2.0 );
        QWheelEvent we( c, fader_->mapToGlobal( c.toPoint() ), QPoint(),
                        QPoint( 0, notches * 120 ), Qt::NoButton,
                        Qt::NoModifier, Qt::NoScrollPhase, false );
        QCoreApplication::sendEvent( fader_, &we );
        return true;
    }

    // MOVE THE REAL SLIDER and let Qt deliver valueChanged, so the handler
    // under test is the production one. setValue() on an unchanged tick emits
    // nothing, so a case asking for a value the fader already holds gets a
    // truthful no-op rather than a phantom edit.
    fader_->setValue( sDbToFader( value ) );
    return true;
}

// AC4.4. ONE NOTCH IS ONE dB, and the step is `sfadercurve.h`'s so the two
// mounts cannot drift. QAbstractSlider's own wheel handling is
// `wheelScrollLines() * singleStep` in SLIDER units, which on this curve is
// between ~1.9 dB at unity and ~5 dB down at -60 -- the arranger head carried
// a comment claiming 1.0 dB per notch that was wrong from the day it was
// written, and both faders now go through the same function instead.
bool SMixerStrip::eventFilter( QObject *watched, QEvent *ev )
{
    if( watched == fader_ && ev->type() == QEvent::Wheel && fader_ ) {
        QWheelEvent *we = static_cast<QWheelEvent *>( ev );
        const int notches = we->angleDelta().y() / 120;
        if( notches != 0 ) {
            const int next = sFaderWheelValue( fader_->value(), notches );
            if( next != fader_->value() )
                fader_->setValue( next );   // its own signal commits
        }
        return true;    // never let the slider apply its own step as well
    }
    return QWidget::eventFilter( watched, ev );
}

// AC4.2. THE TRACK-VERB SUBSET, and only what is genuinely shareable: the
// bodies are `strackgestures`, which is the arranger's own code since M4 --
// see that header for the measured list of what does NOT extract (indent and
// outdent resolve the preceding sibling through the arranger's ROW list, and
// a mixer has no rows).
//
// The SUBMITTER is this strip's own `submit_`, which stamps `rootName_` (D12).
// `stimeline::submitActive` would stamp whichever editor TAB is in front, and
// a pane showing a different arrangement would then resolve an empty path and
// silently do NOTHING -- no refusal, no log line.
bool SMixerStrip::runMenuCommand( const QString &command )
{
    STrack *t = track_.data();
    SStdMixer *m = mixer_.data();
    if( !t || !m ) return false;
    // A SYSTEM LANE has no summing parent and no path of the ordinary shape;
    // proposal 45 D6 refuses remove / move / reparent on one, by accident at
    // the verb and on purpose at the check. Refusing here as well keeps the
    // menu from OFFERING something that would be declined -- D6's own "a bound
    // is announced, never silent", read forwards.
    if( t->systemRole() != SSystemRole::None ) return false;

    const QList<STrack *> targets = strackgestures::structuralTargets( m, t );
    if( targets.isEmpty() ) return false;
    const auto submit = [this]( SAction *a ) { submit_( a ); };

    if( command == QLatin1String( "remove-track" ) )
        return strackgestures::removeTracks( m, targets, submit );
    if( command == QLatin1String( "group-track" ) )
        return strackgestures::groupTracks( m, targets, submit );
    if( command == QLatin1String( "ungroup-track" ) )
        return strackgestures::ungroupTracks( m, targets, submit );
    return false;
}

// The menu itself is HAND-VERIFIED, as every menu in this repo is: there is no
// testkit verb for a context menu anywhere here, so each item calls
// `runMenuCommand()` and the gate drives that -- the item's own code path.
void SMixerStrip::contextMenuEvent( QContextMenuEvent *ev )
{
    STrack *t = track_.data();
    if( !t || t->systemRole() != SSystemRole::None ) { ev->ignore(); return; }

    const int n = strackgestures::structuralTargets( mixer_.data(), t ).size();
    const QString sfx = n > 1 ? QStringLiteral( " (%1 tracks)" ).arg( n )
                              : QString();
    QMenu menu( this );
    menu.addAction( tr( "Remove track" ) + sfx,
                    this, [this]{ runMenuCommand( QStringLiteral( "remove-track" ) ); } );
    menu.addAction( n > 1 ? tr( "Group %1 tracks" ).arg( n )
                          : tr( "Group track" ),
                    this, [this]{ runMenuCommand( QStringLiteral( "group-track" ) ); } );
    QAction *ung = menu.addAction(
        tr( "Ungroup track" ),
        this, [this]{ runMenuCommand( QStringLiteral( "ungroup-track" ) ); } );
    ung->setEnabled( !t->childLinks().isEmpty() );
    menu.exec( ev->globalPos() );
    ev->accept();
}

QString SMixerStrip::describe() const
{
    STrack *t = track_.data();
    const double db = t ? t->getVolume() : 0.0;
    return QStringLiteral(
               "name=%1|narrow=%2|mute=%3|solo=%4|arm=%5|db=%6|role=%7"
               "|faderDb=%8|inserts=%9|sends=%10|compact=%11|live=%12"
               "|color=%13|meter=%14" )
        .arg( t ? t->getSName() : QStringLiteral( "?" ) )
        .arg( narrow_ ? 1 : 0 )
        .arg( t && t->isMuted() ? 1 : 0 )
        .arg( t && t->isSolo() ? 1 : 0 )
        .arg( t && t->isArmedForRecording() ? 1 : 0 )
        .arg( db, 0, 'f', 1 )
        .arg( QString::fromLatin1(
            systemRoleToString( t ? t->systemRole() : SSystemRole::None ) ) )
        // THE FADER'S OWN POSITION, which is NOT the same number as `db=`
        // above. That one is the model's volume; this is what the widget
        // shows. A Read-family lane moves the fader WITHOUT editing the model
        // (D11a's second obligation), so the two legitimately differ and only
        // this one can gate the read-value pump.
        .arg( fader_ ? sFaderToDb( fader_->value() ) : 0.0, 0, 'f', 1 )
        .arg( inserts_ && !inserts_->isHidden() ? 1 : 0 )
        .arg( sends_ && !sends_->isHidden() ? 1 : 0 )
        // D5a: BOTH mounted widgets are compact in a mixer column, and the
        // Track Detail dock keeps the full row. Reported as one field because
        // the strip always sets them together.
        .arg( inserts_ && inserts_->isCompact()
              && sends_ && sends_->isCompact() ? 1 : 0 )
        // D11b's SECOND term, reported separately from audibility because it
        // IS separate: a live-owned lane is still audible in every other
        // sense, and the meter reads its pre-FX input rather than going dark.
        // `SLevelMeter::describe()` carries no label, so this is the only way
        // a case can tell the live branch from an ordinary probe that happened
        // to succeed.
        .arg( t && t->isLiveOwnedLane() ? 1 : 0 )
        // AC4.3, and it is what makes the agreement ASSERTABLE rather than
        // merely intended: `colorMatchesArranger` in the seam compares this
        // against `sClipBodyOf()`, the classifier the arranger's own PIXEL
        // gates use.
        .arg( headerColor_.name() )
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
