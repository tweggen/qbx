#include "app/mixerui/smixerpane.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QStringList>

#include "app/mixerui/smixerstrip.h"
#include "app/shell/sapplication.h"
#include "tw/core/twlog.h"
#include "app/objects/mixer/slaneorder.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/model/sproject.h"
#include "app/servicesui/soptions.h"
#include "app/shell/ssettings.h"
#include "app/objects/track/strack.h"

SMixerPane::SMixerPane( QWidget *parent ) : QWidget( parent )
{
    // NO EXPLICIT MINIMUM ON ANYTHING THAT CARRIES A LAYOUT (CONTRACT inv. 6).
    QHBoxLayout *outer = new QHBoxLayout( this );
    outer->setContentsMargins( 2, 2, 2, 2 );
    outer->setSpacing( 4 );

    scroll_ = new QScrollArea( this );
    scroll_->setObjectName( QStringLiteral( "mixerPaneScroll" ) );
    scroll_->setWidgetResizable( true );
    scroll_->setFrameShape( QFrame::NoFrame );
    scroll_->setVerticalScrollBarPolicy( Qt::ScrollBarAlwaysOff );

    stripHost_ = new QWidget( scroll_ );
    stripLayout_ = new QHBoxLayout( stripHost_ );
    stripLayout_->setContentsMargins( 0, 0, 0, 0 );
    stripLayout_->setSpacing( 4 );
    stripLayout_->addStretch( 1 );
    scroll_->setWidget( stripHost_ );
    outer->addWidget( scroll_, 1 );

    // THE MASTER COLUMN IS PINNED AND DOES NOT SCROLL (D6). D1's walk puts
    // the master at the tail; in a horizontal pane the tail is the right-hand
    // end, which is where both reference DAWs put it, and it is the one strip
    // a user needs while looking at any other -- so it lives in a separate,
    // non-scrolling container beside the scroll area rather than inside it.
    masterHost_ = new QWidget( this );
    masterLayout_ = new QHBoxLayout( masterHost_ );
    masterLayout_->setContentsMargins( 0, 0, 0, 0 );
    masterLayout_->setSpacing( 0 );
    outer->addWidget( masterHost_, 0 );

    connect( &SApplication::app(), &SApplication::meterTick,
             this, [this]( offset_t pos, qint64 nowMs, bool live ) {
                 onMeterTick( pos, nowMs, live );
             } );
}

SMixerPane::~SMixerPane() = default;

// The app BROADCASTS and the PANE connects once, rather than every strip
// connecting itself. Two reasons, and the first is a contract:
//
//  - inv. 5 requires the DOCK gate to be answered before the model walk, and a
//    gate inside each strip has already walked by the time it runs;
//  - one connection instead of N, dropped when the pane dies rather than N
//    times as strips are rebuilt.
int SMixerPane::onMeterTick( offset_t pos, qint64 nowMs, bool live )
{
    // THE DOCK GATE. `isHidden()` rather than `!isVisible()`, deliberately: a
    // widget whose window has never been shown is not "visible" either, and in
    // a --test-case run NOTHING is — so `!isVisible()` would make this return
    // for every scripted run and the meters would be ungateable. `isHidden()`
    // asks the question that was actually meant: has somebody hidden it.
    if( isHidden() ) return 0;

    int worked = 0;
    for( SMixerStrip *s : strips_ )
        if( s->onMeterTick( pos, nowMs, live ) ) ++worked;
    if( masterStrip_ && masterStrip_->onMeterTick( pos, nowMs, live ) ) ++worked;
    tickWork_ += worked;
    return worked;
}

void SMixerPane::clearStrips_()
{
    for( SMixerStrip *s : strips_ ) { s->setParent( nullptr ); s->deleteLater(); }
    strips_.clear();
    if( masterStrip_ ) {
        masterStrip_->setParent( nullptr );
        masterStrip_->deleteLater();
        masterStrip_ = nullptr;
    }
}

void SMixerPane::setRoot( SObject *root, const QString &rootName )
{
    // IDEMPOTENT, and that is load-bearing rather than an optimisation.
    // `rebuildStrips()` destroys every strip, and with it every meter's
    // ballistics and every twLevelProbe's window. The test seams call this on
    // ENTRY so a pane can never answer from a stale root (inv. 60's shape), so
    // without this guard a `mixer-meter-tick` would push a level into strips
    // that the very next `assert-mixer-pane` throws away — measured: the probe
    // read peak 0.399994 and the widget reported -60 dB.
    //
    // A second `load-project` hands over a genuinely different root and still
    // rebuilds, which is the case the entry call exists for.
    if( mixer_.data() == dynamic_cast<SStdMixer *>( root )
        && rootName_ == rootName && !strips_.isEmpty() )
        return;

    rootName_ = rootName;
    if( SStdMixer *old = mixer_.data() )
        for( const QMetaObject::Connection &c : structureConns_ )
            QObject::disconnect( c );
    structureConns_.clear();

    mixer_ = dynamic_cast<SStdMixer *>( root );

    // REBUILD ON STRUCTURE, UPDATE IN PLACE OTHERWISE (CONTRACT inv. 7). A
    // pane that rebuilt on every model signal would delete the fader under
    // the hand mid-drag -- the arranger's own head rebuild uses deleteLater()
    // and says why. Structure is: tracks added, removed, reordered, or a
    // lane's HIDDEN flag changed (which changes the strip list, D2/D6a);
    // volume, mute, solo and arm are per-track signals the STRIP handles.
    if( SStdMixer *m = mixer_.data() ) {
        structureConns_ << connect( m, &SStdMixer::trackInserted,
                                    this, [this]{ rebuildIfStructureChanged(); } )
                        << connect( m, &SStdMixer::trackRemoved,
                                    this, [this]{ rebuildIfStructureChanged(); } )
                        << connect( m, &SStdMixer::tracksReordered,
                                    this, &SMixerPane::rebuildIfStructureChanged );
        // `arrangementChanged` fires from the action chokepoint after EVERY
        // action, so it is not a structure signal -- it is "something
        // happened". It is connected anyway because the strip list also
        // changes for things no signal reports (a send lane added or removed,
        // a lane hidden), and it goes to the GUARDED rebuild for exactly that
        // reason.
        if( SProject *p = m->getProjectSafe() )
            structureConns_ << connect( p, &SProject::arrangementChanged,
                                        this, &SMixerPane::rebuildIfStructureChanged );
    }
    rebuildStrips();
}

// REBUILD ON STRUCTURE, UPDATE IN PLACE OTHERWISE -- CONTRACT inv. 7, which
// the M2 wiring quietly violated by connecting `arrangementChanged` straight
// to `rebuildStrips()`. That signal fires from the action chokepoint after
// EVERY action, so every verb destroyed and rebuilt every strip: the
// fader-under-the-hand hazard inv. 7 names was live, and M3 found it because a
// rebuilt strip also throws away its meter's ballistics and its probe's window
// (measured: the probe read peak 0.399994 and the widget reported -60 dB one
// line later).
//
// The signal is KEPT -- the strip list genuinely changes for things no signal
// reports, a send lane added or a lane hidden -- and the WALK decides. A no-op
// costs one walk, which is the same walk a rebuild would do anyway, and no
// widget churn at all.
void SMixerPane::rebuildIfStructureChanged()
{
    SStdMixer *mixer = mixer_.data();
    if( !mixer ) return;

    const QVector<slaneorder::Lane> lanes =
        slaneorder::flattenTrackLanes( mixer, walkOptions() );

    int i = 0;
    bool same = true;
    for( const slaneorder::Lane &lane : lanes ) {
        SMixerStrip *st = stripAt( i++ );
        if( !st || st->track() != lane.track ) { same = false; break; }
    }
    if( same && i == strips_.size() + ( masterStrip_ ? 1 : 0 ) ) return;
    rebuildStrips();
}

slaneorder::Options SMixerPane::walkOptions()
{
    slaneorder::Options opt;
    opt.fold             = slaneorder::Fold::Ignore;
    opt.hidden           = slaneorder::Hidden::Honour;
    opt.system           = slaneorder::SystemLanes::MasterSubtree;
    opt.alwaysShowMaster = true;
    return opt;
}

void SMixerPane::rebuildStrips()
{
    clearStrips_();
    // A pane built now must honour the stored preference, not the defaults it
    // was constructed with: every test seam builds a FRESH pane (shell inv. 62)
    // and so does a project open.
    const int m = sectionsMask();
    showInserts_ = m & 1; showSends_ = m & 2;
    showMeter_   = m & 4; showFader_ = m & 8;
    SStdMixer *mixer = mixer_.data();
    if( !mixer ) return;

    // ONE WALK, SHARED WITH THE ARRANGER (M0 / D1). The options ARE the
    // pane's semantics, stated rather than implied:
    //
    //   Fold::Ignore     a collapsed folder's children KEEP their strips --
    //                    fold answers "show this lane's children as ROWS",
    //                    and a mixer has no rows (D2). Consequence to know
    //                    rather than rediscover: the strip count does not
    //                    match the arranger's visible row count, and no gate
    //                    may assert that it does.
    //   Hidden::Honour   hiding answers "show this track at all", which both
    //                    mounts must agree on.
    //   alwaysShowMaster D6a -- the ONE place the two mounts deliberately
    //                    disagree about a model flag. Every system role is
    //                    laneHiddenByDefault(), so on a project nobody has
    //                    touched the master lane has no arranger row; a mixer
    //                    without its summing point is not a mixer. The
    //                    exemption covers the master ITSELF and never its
    //                    children, so a hidden conductor lane stays hidden.
    for( const slaneorder::Lane &lane :
             slaneorder::flattenTrackLanes( mixer, walkOptions() ) ) {
        if( !lane.track ) continue;
        const bool isMaster = lane.role == SSystemRole::Master;
        QWidget *host = isMaster ? masterHost_ : stripHost_;
        SMixerStrip *strip = new SMixerStrip( mixer, lane.track, rootName_, host );
        strip->setSectionsVisible( showInserts_, showSends_, showMeter_, showFader_ );
        if( isMaster ) {
            masterLayout_->addWidget( strip, 0 );
            masterStrip_ = strip;
        } else {
            // Before the trailing stretch, so the strips pack left.
            stripLayout_->insertWidget( stripLayout_->count() - 1, strip, 0 );
            strips_.append( strip );
        }
    }
}

SMixerStrip *SMixerPane::stripAt( int i ) const
{
    if( i >= 0 && i < strips_.size() ) return strips_.at( i );
    if( i == strips_.size() ) return masterStrip_;   // the pinned tail
    return nullptr;
}

SMixerStrip *SMixerPane::stripForTrackNamed( const QString &name ) const
{
    for( SMixerStrip *s : strips_ )
        if( s->track() && s->track()->getSName() == name ) return s;
    if( masterStrip_ && masterStrip_->track()
        && masterStrip_->track()->getSName() == name ) return masterStrip_;
    return nullptr;
}

// THE SECTION MASK IS PER-USER AND LIVES IN SOpt (proposal 48 M2 / D9):
// 1 inserts | 2 sends | 4 meter | 8 fader. It is NOT undoable -- a preference
// is not an edit to the arrangement, the call `set-count-in` and
// `set-pre-roll` already make.
int SMixerPane::sectionsMask()
{
    return SSettings::instance()
        .value( SOpt::MixerSections, SOpt::def( SOpt::MixerSections ) ).toInt();
}

void SMixerPane::setSectionsMask( int mask )
{
    SSettings::instance().setValue( SOpt::MixerSections, mask );
}

void SMixerPane::applyStoredSections()
{
    const int m = sectionsMask();
    setSectionsVisible( m & 1, m & 2, m & 4, m & 8 );
}

void SMixerPane::setSectionsVisible( bool inserts, bool sends,
                                     bool meter, bool fader )
{
    showInserts_ = inserts;
    showSends_   = sends;
    showMeter_   = meter;
    showFader_   = fader;
    for( SMixerStrip *s : strips_ )
        s->setSectionsVisible( inserts, sends, meter, fader );
    if( masterStrip_ )
        masterStrip_->setSectionsVisible( inserts, sends, meter, fader );
}

QString SMixerPane::describe() const
{
    QStringList names;
    for( SMixerStrip *s : strips_ )
        names << ( s->track() ? s->track()->getSName() : QStringLiteral( "?" ) );
    if( masterStrip_ && masterStrip_->track() )
        names << masterStrip_->track()->getSName();
    return QStringLiteral( "strips=%1|master=%2|sections=%3|tickWork=%4|names=%5" )
        .arg( strips_.size() + ( masterStrip_ ? 1 : 0 ) )
        .arg( masterStrip_ ? 1 : 0 )
        .arg( sectionsMask() )
        .arg( tickWork_ )
        .arg( names.join( QLatin1Char( ',' ) ) );
}

// D13. The pane holds one STrack* and one twLevelProbe PER STRIP, and
// closeProject() deletes the project AFTER destroyDocksToolbars(). Without
// this the next 33 ms meter tick dereferences freed tracks -- a crash, not a
// glitch, and the same class as the SCut revalidation UAF and the SViewTabs
// dangling-root hazard, both of which this tree fixed by wiring lifetime from
// the first commit rather than later.
void SMixerPane::detachProject()
{
    for( SMixerStrip *s : strips_ ) s->detachProject();
    if( masterStrip_ ) masterStrip_->detachProject();
    clearStrips_();
    mixer_ = nullptr;
}
