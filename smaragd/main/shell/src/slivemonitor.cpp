#include "app/shell/slivemonitor.h"

#include <algorithm>

#include <QTimer>

#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "app/shell/sapplication.h"
#include "app/shell/sliveinputsource.h"
#include "app/shell/ssettings.h"
#include "tw/core/twlog.h"
#include "tw/devices/audio_input.h"
#include "tw/graph/twcomponent.h"
#include "tw/mix/twrewire.h"
#include "tw/mix/twtrackmix.h"
#include "tw/plugins/twpluginchain.h"
#include "tw/pages/tw_output_page.h"
#include "tw/playback/twlivepump.h"
#include "tw/playback/twspeaker.h"
#include "tw/plugins/twpluginslotproc.h"
#include "tw/schedule/capture_revalidator.h"

namespace {

// The frozen-input roots of a plan, de-duplicated. One demand handle per root
// is design D3's rule; two folders sharing a child would otherwise get two.
std::vector<std::shared_ptr<twComponent> >
frozenRootsOf( const std::shared_ptr<const twLivePlan> &plan )
{
    std::vector<std::shared_ptr<twComponent> > out;
    if( !plan ) return out;
    for( const twLiveTrackPlan &t : plan->tracks )
        for( const std::shared_ptr<twComponent> &c : t.frozenInputs )
            if( c && std::find( out.begin(), out.end(), c ) == out.end() )
                out.push_back( c );
    return out;
}

}  // namespace

SLiveMonitor::SLiveMonitor( SApplication *app )
    : QObject( app ), app_( app )
{
    disarmTimer_ = new QTimer( this );
    disarmTimer_->setSingleShot( true );
    disarmTimer_->setInterval( kDisarmTailMs );
    connect( disarmTimer_, &QTimer::timeout, this, &SLiveMonitor::finishDisarm );

    demandTimer_ = new QTimer( this );
    demandTimer_->setInterval( kDemandTickMs );
    connect( demandTimer_, &QTimer::timeout, this, &SLiveMonitor::pumpDemands );
}

SLiveMonitor::~SLiveMonitor()
{
    // The pump is a std::thread. Joining it HERE, on the main thread and while
    // the log sink is alive, is the same discipline SMidiOutPump's scheduler
    // threads follow -- a join during static destruction is the deadlock this
    // project has already paid for once.
    stopPump();
    setClosureOwned( current_, false );
    setClosureOwned( departing_, false );
    if( input_ ) {
        input_->stopCapture();
        input_->closeDevice();
        input_.reset();
    }
    if( liveOpened_ ) {
        if( std::shared_ptr<twSpeaker> spk = app_ ? app_->getSpeaker()
                                                  : std::shared_ptr<twSpeaker>() )
            spk->closeLive();
        liveOpened_ = false;
    }
}

// --- helpers ----------------------------------------------------------------

SStdMixer *SLiveMonitor::rootMixer() const
{
    SProject *p = app_ ? app_->getCurrentProject() : nullptr;
    return p ? dynamic_cast<SStdMixer *>( p->getRootComponent() ) : nullptr;
}

std::uint64_t SLiveMonitor::rootEpoch() const
{
    SStdMixer *m = rootMixer();
    if( !m ) return 0;
    const std::shared_ptr<twRewire> r = m->masterRewireComponent();
    return r ? r->contentEpochNow() : 0;
}

void SLiveMonitor::applyExclusion()
{
    SStdMixer *m = rootMixer();
    if( !m ) return;
    // ONE top-down pass, exactly as a solo flip gets: the mixer nulls a
    // top-level member's plug, a folder setClipMuted's a nested one. Then the
    // whole render chain is staled -- rewiring only changes what FUTURE
    // freezes produce, and the pages already frozen still contain (or still
    // lack) the track (the mute precedent, sstdmixer.cpp).
    m->applyAudibility();
    m->invalidateRenderPath();
}

void SLiveMonitor::retireClosureNodes( const SLiveClosure &closure )
{
    SProject *p = app_ ? app_->getCurrentProject() : nullptr;
    CaptureRevalidator *sched = p ? p->getRevalidator() : nullptr;
    if( !sched || closure.empty() ) return;

    std::vector<const twComponent *> comps;
    for( STrack *t : closure.ordered ) {
        if( t->trackMixComponent() )    comps.push_back( t->trackMixComponent().get() );
        if( t->pluginChainComponent() ) comps.push_back( t->pluginChainComponent().get() );
        if( t->gainStageComponent() )   comps.push_back( t->gainStageComponent().get() );
        if( t->getRootComponent() )     comps.push_back( t->getRootComponent().get() );
    }
    sched->retireComponentNodes( comps );
}

void SLiveMonitor::setClosureOwned( const SLiveClosure &closure, bool owned )
{
    for( STrack *t : closure.ordered ) {
        SPluginChain *chain = t->getPluginChain();
        if( !chain ) continue;
        const int n = chain->getSlotCount();
        for( int i = 0; i < n; ++i ) {
            SPluginSlot *slot = chain->getSlotAt( i );
            if( !slot ) continue;
            const std::shared_ptr<audio::twPluginSlotProcessor> &proc = slot->getProcessor();
            if( !proc ) continue;
            if( !owned ) proc->forgetContinuity();
            proc->setLiveOwned( owned );
        }
    }
}

bool SLiveMonitor::ensureInput( STrack *track )
{
    if( !track ) return false;
    QString want = track->trackInputAudioDevice();
    if( want.isEmpty() ) want = SSettings::instance().audioInputDeviceId();
    if( want.isEmpty() ) want = QStringLiteral( "default" );

    if( input_ && inputDeviceId_ == want ) return true;
    if( input_ ) {
        input_->stopCapture();
        input_->closeDevice();
        input_.reset();
    }

    // Selected by SMARAGD_AUDIO_INPUT_BACKEND ahead of the platform (L0), so a
    // headless case replays a WAV through the same capture thread and ring a
    // device uses.
    std::unique_ptr<audio::AudioInput> in = audio::createAudioInput();
    if( !in ) return false;
    const int rate = app_->get303aEnvironment() ? app_->get303aEnvironment()->getSRate()
                                                : 48000;
    if( in->openDevice( want.toStdString(), (std::uint32_t) rate ) != 0 ) {
        TW_LOGW( "shell", "[LIVE] input device '%s' would not open: %s",
                 want.toStdString().c_str(), in->errorMessage() );
        return false;
    }
    if( in->startCapture() != 0 ) {
        TW_LOGW( "shell", "[LIVE] input device '%s' would not start: %s",
                 want.toStdString().c_str(), in->errorMessage() );
        in->closeDevice();
        return false;
    }
    input_         = std::move( in );
    inputDeviceId_ = want;
    TW_LOGI( "shell", "[LIVE] input open: backend=%s device='%s' %u ch @ %u Hz, "
                      "reported latency %u frames",
             input_->backendName(), want.toStdString().c_str(),
             input_->getConfig().channels, input_->getConfig().sampleRate,
             input_->getLatencyFrames() );
    return true;
}

void SLiveMonitor::closeInputIfUnused()
{
    if( !current_.empty() || !departing_.empty() ) return;
    if( !input_ ) return;
    input_->stopCapture();
    input_->closeDevice();
    input_.reset();
    inputDeviceId_.clear();
}

void SLiveMonitor::ensurePump()
{
    if( pump_ ) return;
    std::shared_ptr<twSpeaker> spk = app_->getSpeaker();
    if( !spk ) return;
    pump_.reset( new LiveGraphPump( spk->liveRing(), spk->engineClock() ) );
}

void SLiveMonitor::stopPump()
{
    if( !pump_ ) return;
    pump_->stop();
    pump_.reset();
}

// --- the plan ---------------------------------------------------------------

void SLiveMonitor::publishPlan( const SLiveClosure &closure,
                                std::uint64_t flipEpoch,
                                std::uint64_t flipEpochPrime )
{
    std::shared_ptr<twSpeaker> spk = app_->getSpeaker();
    if( !spk ) return;

    SLivePlanBuilder::Params p;
    p.mixer       = rootMixer();
    p.sampleRate  = app_->get303aEnvironment() ? app_->get303aEnvironment()->getSRate()
                                               : 48000;
    p.width       = (idx_t) app_->masterChannels();
    p.playing     = app_->isPlaying();
    p.recording   = app_->isRecordingActive();
    p.locator     = app_->getGlobalLocatorPos();
    p.flipEpoch      = flipEpoch;
    p.flipEpochPrime = flipEpochPrime;
    if( audio::AudioBackend *b = spk->getBackend() )
        p.blockFrames = (length_t) b->getConfig().bufferFrames;
    if( p.blockFrames <= 0 ) p.blockFrames = 1024;

    // One source object per source track, rebuilt with the plan: the scratch
    // and the channel map are sized HERE, on the main thread, so the pump's
    // pull() allocates nothing.
    sources_.clear();
    SLivePlanBuilder::SourceFn fn =
        [this, &closure, &p]( STrack *t ) -> std::shared_ptr<twLiveInputSource> {
            if( !ensureInput( t ) ) return nullptr;
            auto src = std::make_shared<SLiveAudioInputSource>(
                input_.get(), t->trackInputChannelMask(),
                (idx_t) t->getChannels(), p.blockFrames );
            sources_.push_back( src );
            return src;
        };

    std::shared_ptr<twLivePlan> plan = SLivePlanBuilder::build( closure, p, fn );
    if( !plan ) {
        if( pump_ ) pump_->setPlan( nullptr );
        return;
    }
    ensurePump();
    if( !pump_ ) return;
    pump_->setPlan( plan );
    pump_->start();
}

// --- the triggers -----------------------------------------------------------

void SLiveMonitor::refresh()
{
    if( !app_ || suspendedForRender_ ) return;
    SStdMixer *mixer = rootMixer();
    if( !mixer ) return;

    const SLiveClosure want = sliveplan::computeClosure(
        mixer, app_->isPlaying(), app_->isRecordingActive(), inertlyArmed_ );

    const bool sameSet = ( want.ordered == current_.ordered )
                         && ( want.sources == current_.sources );
    if( sameSet ) {
        // Nothing structural moved: only the transport, the fader or an insert
        // did. Rebuild and republish -- a plan is a SNAPSHOT, so a fader move
        // on a closure member is only heard once a new one is built (design
        // section 3's rebuild triggers).
        if( !current_.empty() ) publishPlan( current_, rootEpoch(), 0 );
        return;
    }

    // --- the DISARM half, first: members that are leaving ------------------
    SLiveClosure leaving;
    for( STrack *t : current_.ordered )
        if( !want.contains( t ) ) leaving.ordered.push_back( t );

    if( !leaving.ordered.empty() ) {
        // A second disarm while a tail is in flight finishes the first one
        // synchronously: two overlapping tails would race over setLiveOwned.
        if( disarmTimer_->isActive() ) { disarmTimer_->stop(); finishDisarm(); }

        retireClosureNodes( leaving );
        for( STrack *t : leaving.ordered ) t->setLiveOwnedLane( false );
        applyExclusion();
        const std::uint64_t flipPrime = rootEpoch();

        // THE TAIL: the departing members are still rendered and still
        // live-owned, and the entry carries flipEpochPrime, so the RT keeps
        // summing the ring while the root page it serves still LACKS them and
        // stops the moment the re-summed one lands (design D2).
        departing_ = current_;
        publishPlan( current_, 0, flipPrime );
        disarmTimer_->start();
    }

    // --- the ARM half ------------------------------------------------------
    SLiveClosure arriving;
    for( STrack *t : want.ordered )
        if( !current_.contains( t ) ) arriving.ordered.push_back( t );

    current_ = want;

    if( want.empty() ) {
        demandTimer_->stop();
        demands_.clear();
        return;
    }

    if( !arriving.ordered.empty() ) {
        // 1. drain, 2. own, 3. wire + bump, 4. read the epoch.
        retireClosureNodes( arriving );
        setClosureOwned( arriving, true );
        for( STrack *t : arriving.ordered ) t->setLiveOwnedLane( true );
        applyExclusion();
    }
    const std::uint64_t flipEpoch = rootEpoch();

    // 5. the device. openLive() REFUSES a device rate that is not the project
    //    rate: the ring is stamped in PROJECT frames and the RT sums it
    //    straight into the device buffer, so the two only line up while the
    //    rates are equal. Refusing loudly beats monitoring at the wrong pitch.
    if( !liveOpened_ ) {
        std::shared_ptr<twSpeaker> spk = app_->getSpeaker();
        const int rate = app_->get303aEnvironment()
                             ? app_->get303aEnvironment()->getSRate() : 48000;
        if( spk && spk->openLive( (std::uint32_t) rate,
                                  (idx_t) app_->masterChannels() ) == 0 ) {
            liveOpened_ = true;
            lastRefusal_.clear();
        } else {
            lastRefusal_ = QStringLiteral(
                "the audio device will not run at the project rate (%1 Hz); "
                "monitoring is off" ).arg( rate );
            TW_LOGW( "shell", "[LIVE] %s", lastRefusal_.toStdString().c_str() );
            // The exclusion stays UNDONE: a track nobody can monitor must keep
            // being heard from its own clips.
            for( STrack *t : current_.ordered ) t->setLiveOwnedLane( false );
            setClosureOwned( current_, false );
            applyExclusion();
            current_ = SLiveClosure();
            return;
        }
    }

    // 6. publish, then one explicit reposition.
    publishPlan( current_, flipEpoch, 0 );
    if( pump_ ) pump_->requestReposition();
    if( !demandTimer_->isActive() ) demandTimer_->start();
    pumpDemands();
    // Meters keep ticking while a live lane is ON, at a standing playhead.
    app_->startMetering();
}

void SLiveMonitor::finishDisarm()
{
    if( departing_.empty() ) return;
    SLiveClosure gone;
    for( STrack *t : departing_.ordered )
        if( !current_.contains( t ) ) gone.ordered.push_back( t );
    departing_ = SLiveClosure();

    // The plan that drops them has been published (or the pump stopped), so
    // release ownership and let the frozen path have the chain back.
    if( current_.empty() ) {
        stopPump();
    } else {
        publishPlan( current_, rootEpoch(), 0 );
        if( pump_ ) pump_->requestReposition();
    }
    setClosureOwned( gone, false );

    // The SECOND bump. A page frozen during the tail would have been SILENT
    // for the departing track (the ownership guard answers silence and counts),
    // and it would carry the current epoch, so it would be adopted as valid and
    // the track would stay silent until the next unrelated edit. Staling it is
    // what makes the overlap self-healing rather than audible.
    if( SStdMixer *m = rootMixer() ) m->invalidateRenderPath();

    if( current_.empty() ) {
        demandTimer_->stop();
        demands_.clear();
        closeInputIfUnused();
        if( liveOpened_ ) {
            if( std::shared_ptr<twSpeaker> spk = app_->getSpeaker() ) spk->closeLive();
            liveOpened_ = false;
        }
    }
}

void SLiveMonitor::transportChanged()
{
    refresh();
    if( pump_ ) pump_->requestReposition();
}

void SLiveMonitor::seeked()
{
    if( !pump_ ) return;
    // A seek changes the position the pump must render at, and while STOPPED it
    // also changes the anchor and the automation hold -- both live in the plan.
    if( !current_.empty() ) publishPlan( current_, rootEpoch(), 0 );
    pump_->requestReposition();
}

void SLiveMonitor::suspendForRender()
{
    if( suspendedForRender_ || ( current_.empty() && departing_.empty() ) ) {
        suspendedForRender_ = true;
        return;
    }
    // A render suspends every live lane for its duration (design D4): export
    // ignores the split, as in Cubase, and `beginRun` therefore never meets a
    // live-owned track. Everything is undone, in the disarm order, and NOW --
    // a render must not start while a tail is still in flight.
    if( disarmTimer_->isActive() ) { disarmTimer_->stop(); finishDisarm(); }

    suspended_ = current_;
    stopPump();
    setClosureOwned( current_, false );
    for( STrack *t : current_.ordered ) t->setLiveOwnedLane( false );
    current_ = SLiveClosure();
    applyExclusion();
    demandTimer_->stop();
    demands_.clear();
    closeInputIfUnused();
    if( liveOpened_ ) {
        if( std::shared_ptr<twSpeaker> spk = app_->getSpeaker() ) spk->closeLive();
        liveOpened_ = false;
    }
    suspendedForRender_ = true;
}

void SLiveMonitor::resumeAfterRender()
{
    if( !suspendedForRender_ ) return;
    suspendedForRender_ = false;
    suspended_ = SLiveClosure();
    // A FRESH arm, never a resume: the closure is recomputed from the model as
    // it now stands, and the whole retire/own/wire/epoch sequence runs again.
    refresh();
}

void SLiveMonitor::projectChanged()
{
    // Arms that arrived in a project file are INERT (design D9). The flag
    // itself is untouched -- it round-trips through the file exactly as it
    // did -- but it does not make a track a monitoring source until the user
    // arms it in THIS session, which is what `arm-track` clears.
    inertlyArmed_.clear();
    if( SStdMixer *mixer = rootMixer() ) {
        const SLiveClosure all =
            sliveplan::computeClosure( mixer, false, false, {} );
        for( STrack *t : all.sources )
            if( t->isArmedForRecording() ) inertlyArmed_.push_back( t );
    }
    refresh();
}

// --- the re-rooted horizon demands (design D3) ------------------------------

void SLiveMonitor::pumpDemands()
{
    if( !pump_ || current_.empty() ) return;
    SProject *p = app_ ? app_->getCurrentProject() : nullptr;
    CaptureRevalidator *sched = p ? p->getRevalidator() : nullptr;
    if( !sched ) return;

    const std::vector<std::shared_ptr<twComponent> > roots =
        frozenRootsOf( pump_->plan() );
    if( roots.empty() ) { demands_.clear(); return; }

    // Every UNARMED sibling under an excluded folder is rendered by the PUMP
    // from its frozen root pages, and the pump never demands (threading table,
    // section 4). So the horizon is re-rooted here: one handle per root, on the
    // main thread, superseded per handle by replacing it.
    const offset_t page = (offset_t) twOutputPage::FRAME_CAPACITY;
    const offset_t pos  = app_->getGlobalLocatorPos();
    const offset_t start = ( pos / page ) * page;

    demands_.resize( roots.size() );
    for( std::size_t i = 0; i < roots.size(); ++i )
        demands_[i] = sched->requestGraphPages( roots[i], start, kDemandPages, 9 );
}

// --- state ------------------------------------------------------------------

bool SLiveMonitor::active() const
{
    return !current_.empty() && pump_ && pump_->running();
}

double SLiveMonitor::inputPeak( const STrack *track ) const
{
    for( std::size_t i = 0; i < current_.sources.size() && i < sources_.size(); ++i )
        if( current_.sources[i] == track && sources_[i] )
            return sources_[i]->peekPeak();
    return 0.0;
}

std::uint64_t SLiveMonitor::rateRefusals() const
{
    std::shared_ptr<twSpeaker> spk = app_ ? app_->getSpeaker()
                                          : std::shared_ptr<twSpeaker>();
    return spk ? spk->liveRateRefusals() : 0;
}

QString SLiveMonitor::describe() const
{
    QString s = QStringLiteral( "live=%1 tracks=%2 sources=%3" )
                    .arg( active() ? "on" : "off" )
                    .arg( current_.ordered.size() )
                    .arg( current_.sources.size() );
    if( input_ )
        s += QStringLiteral( " input=%1:%2" )
                 .arg( QString::fromUtf8( input_->backendName() ) )
                 .arg( inputDeviceId_ );
    if( pump_ )
        s += QStringLiteral( " blocks=%1 repositions=%2 misses=%3 shortfalls=%4" )
                 .arg( pump_->blocks() ).arg( pump_->repositions() )
                 .arg( pump_->frozenInputMisses() ).arg( pump_->inputShortfalls() );
    if( !lastRefusal_.isEmpty() ) s += QStringLiteral( " refused='%1'" ).arg( lastRefusal_ );
    return s;
}
