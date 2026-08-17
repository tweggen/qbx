#include "app/shell/slivemonitor.h"

#include <algorithm>
#include <cstring>

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
    connect( demandTimer_, &QTimer::timeout, this, &SLiveMonitor::pumpEdits );
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

void SLiveMonitor::applyExclusion( const SLiveClosure &affected )
{
    SStdMixer *m = rootMixer();
    if( !m ) return;
    // ONE top-down pass, exactly as a solo flip gets: the mixer nulls a
    // top-level member's plug, a folder setClipMuted's a nested one. Then the
    // whole render chain is staled -- rewiring only changes what FUTURE
    // freezes produce, and the pages already frozen still contain (or still
    // lack) the track (the mute precedent, sstdmixer.cpp).
    m->applyAudibility();

    // AND IT HAS TO BE PER MEMBER, not one call on the mixer. `SObject::
    // invalidateRenderPath()` walks from the project ROOT and stales every
    // chain CONTAINING the object it is called on - so calling it on the mixer
    // stales the mixer and the root rewire and NOTHING BELOW THEM. The member
    // tracks' own pages would keep being served, and a page frozen while the
    // member was still live-owned is SILENCE for that track: measured as a
    // folder that went quiet at the disarm and never came back.
    for( STrack *t : affected.ordered ) t->invalidateRenderPath();
    if( affected.ordered.empty() ) m->invalidateRenderPath();
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
    publishedSignature_ = planSignature();
}

// --- the triggers -----------------------------------------------------------

void SLiveMonitor::refresh()
{
    if( !app_ || suspendedForRender_ ) return;
    SStdMixer *mixer = rootMixer();
    if( !mixer ) return;

    const bool playing = ( pendingPlaying_ >= 0 ) ? ( pendingPlaying_ != 0 )
                                                  : app_->isPlaying();
    const SLiveClosure want = sliveplan::computeClosure(
        mixer, playing, app_->isRecordingActive(), inertlyArmed_ );

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

    bool tailNow = false;
    if( !leaving.ordered.empty() ) {
        // A second disarm while a tail is in flight finishes the first one
        // synchronously: two overlapping tails would race over setLiveOwned.
        if( disarmTimer_->isActive() ) { disarmTimer_->stop(); finishDisarm(); }

        retireClosureNodes( leaving );
        // OWNERSHIP IS RELEASED BEFORE THE RE-WIRE, and that order is the
        // whole correctness of the hand-back.
        //
        // Releasing it AFTER looks safer and is wrong: the freeze path would
        // regain the chain while the processors were still live-owned, the
        // very next root page would be frozen as SILENCE for those tracks, and
        // the epoch gate would flip the RT onto it - measured as a folder that
        // went quiet for the whole tail. Releasing it here, while the
        // exclusion is still applied, means no freeze can reach the chain yet
        // (planPage skips a nulled plug), so the release is safe AND the first
        // re-summed page carries real audio.
        setClosureOwned( leaving, false );
        for( STrack *t : leaving.ordered ) t->setLiveOwnedLane( false );
        applyExclusion( leaving );
        const std::uint64_t flipPrime = rootEpoch();

        // THE TAIL: the departing members are STILL RENDERED by the pump (on
        // processors it no longer owns exclusively, which is safe for exactly
        // the length of the tail and is the price of a gap-free hand-back),
        // and every entry carries flipEpochPrime - so the RT keeps summing the
        // ring while the root page it serves still LACKS them, and stops the
        // moment the re-summed one lands (design D2).
        departing_ = current_;
        publishPlan( current_, 0, flipPrime );
        // THE TAIL IS ONLY FOR A HAND-BACK THAT SOMEBODY IS LISTENING TO.
        // While the frozen lane is not PLAYING there is no root page being
        // served, so there is nothing for the ring to cover and no reason to
        // hold a processor the freeze path is about to want - and holding it
        // is exactly what would count `liveOwnedRefusals` for no benefit.
        tailNow = !app_->isPlaying();
    }

    // --- the ARM half ------------------------------------------------------
    SLiveClosure arriving;
    for( STrack *t : want.ordered )
        if( !current_.contains( t ) ) arriving.ordered.push_back( t );

    // `current_` MUST be the new set before finishDisarm() runs: that function
    // computes what is really gone as "departing minus current", and closes the
    // device when `current_` is empty. Finishing the tail against the OLD set
    // released nothing and left the device open - which then made every phase
    // of a case share one capture session.
    current_ = want;

    if( !leaving.ordered.empty() ) {
        if( tailNow ) finishDisarm();
        else          disarmTimer_->start();
    }

    if( want.empty() ) {
        demandTimer_->stop();
        demands_.clear();
        return;
    }

    // THE MASTER-SHAPE PRECONDITION, BEFORE ANYTHING IS ARMED (design D3).
    //
    // "root(unarmed) + ring" is exact only while the master is a unity sum
    // followed by an identity map. The plan builder can express the other mode
    // - the master joins the closure and the pump renders it - but the RT half
    // of it is NOT wired: twSpeaker adds the frozen root page whenever the
    // frozen lane is PLAYING, and nothing reads twLivePlan::masterLinear. A
    // Closure-shaped plan would therefore be summed ON TOP of a root page that
    // already contains those tracks, and the user would hear the arrangement
    // doubled.
    //
    // So it is REFUSED rather than approximated: one log line naming the
    // reason, and the arrangement keeps playing untouched. Whoever adds a
    // master insert chain will land here, and the fix is a twSpeaker that
    // stops adding the root page while a non-linear plan is live.
    {
        const twlive::twMasterShape shape = twlive::checkMasterShape(
            mixer->masterMixComponent().get(),
            mixer->masterRewireComponent().get(),
            (idx_t) app_->masterChannels() );
        if( !shape.linear() ) {
            if( lastRefusal_.isEmpty() ) {
                lastRefusal_ = QStringLiteral(
                    "the master is not a unity sum with an identity map (%1); "
                    "live monitoring is off" ).arg( QString::fromUtf8( shape.reason ) );
                TW_LOGW( "shell", "[LIVE] %s", lastRefusal_.toStdString().c_str() );
            }
            current_ = SLiveClosure();
            return;
        }
    }

    if( !arriving.ordered.empty() ) {
        // 1. drain, 2. own, 3. wire + bump, 4. read the epoch.
        retireClosureNodes( arriving );
        setClosureOwned( arriving, true );
        for( STrack *t : arriving.ordered ) t->setLiveOwnedLane( true );
        applyExclusion( arriving );
    }
    const std::uint64_t flipEpoch = rootEpoch();

    // 5. THE INPUT DEVICE, BEFORE the output one. The capture backend clears
    //    its recording at DEVICE start, and FileAudioInput anchors its pacing
    //    at startCapture(), so opening the input second would put a slice of
    //    leading silence into every monitored recording and make it look like
    //    monitoring latency. Opening it first costs nothing and makes
    //    `assert-monitor-latency` measure the thing it is named after.
    for( STrack *t : current_.sources ) ensureInput( t );

    // 6. the output device. openLive() REFUSES a device rate that is not the project
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
            applyExclusion( current_ );
            current_ = SLiveClosure();
            return;
        }
    }

    // 7. publish, then one explicit reposition.
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

    // The tail is over: the pump stops rendering the departing chain. Their
    // ownership was already released when the disarm was requested, so there
    // is nothing to hand back here - only the plan to drop.
    if( current_.empty() ) {
        stopPump();
    } else {
        publishPlan( current_, rootEpoch(), 0 );
        if( pump_ ) pump_->requestReposition();
    }
    setClosureOwned( gone, false );   // idempotent; the belt to the braces above

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

void SLiveMonitor::transportAboutToChange( bool playing )
{
    pendingPlaying_ = playing ? 1 : 0;
    refresh();
}

void SLiveMonitor::transportChanged()
{
    pendingPlaying_ = -1;
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
    SLiveClosure was = current_;
    current_ = SLiveClosure();
    applyExclusion( was );
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

void SLiveMonitor::pumpEdits()
{
    // The rest of design section 3's rebuild triggers, asked rather than
    // wired: a fader move, an insert added / removed / reordered, or an input
    // device change on a closure member all show up as a different signature,
    // and everything else costs one vector compare.
    // NOT while a disarm tail is in flight: the tail plan was published for the
    // OLD closure, so its signature differs from the new one by construction
    // and republishing here would cut the tail short - which is the one thing
    // it exists to prevent.
    if( current_.empty() || suspendedForRender_ || !departing_.empty() ) return;
    const std::vector<std::uintptr_t> sig = planSignature();
    if( sig == publishedSignature_ ) return;
    publishPlan( current_, 0, 0 );
}

// --- state ------------------------------------------------------------------

bool SLiveMonitor::active() const
{
    return !current_.empty() && pump_ && pump_->running();
}

bool SLiveMonitor::isLive( const STrack *track ) const
{
    return track && current_.contains( track ) && pump_ && pump_->running();
}

double SLiveMonitor::inputPeak( const STrack *track ) const
{
    for( std::size_t i = 0; i < current_.sources.size() && i < sources_.size(); ++i )
        if( current_.sources[i] == track && sources_[i] )
            return sources_[i]->peekPeak();
    return 0.0;
}

double SLiveMonitor::takeInputPeak( const STrack *track )
{
    for( std::size_t i = 0; i < current_.sources.size() && i < sources_.size(); ++i )
        if( current_.sources[i] == track && sources_[i] )
            return sources_[i]->takePeak();
    return 0.0;
}

std::vector<std::uintptr_t> SLiveMonitor::planSignature() const
{
    std::vector<std::uintptr_t> sig;
    for( STrack *t : current_.ordered ) {
        sig.push_back( (std::uintptr_t) t );
        sig.push_back( (std::uintptr_t) t->getChannels() );
        if( SPluginChain *chain = t->getPluginChain() ) {
            const int n = chain->getSlotCount();
            for( int i = 0; i < n; ++i ) {
                SPluginSlot *slot = chain->getSlotAt( i );
                sig.push_back( slot ? (std::uintptr_t) slot->getProcessor().get() : 0u );
            }
        }
        if( t->gainStageComponent() ) {
            // The fader, as bits: the pump replays an Envelope SNAPSHOT, so a
            // gain that moved is a plan that is out of date.
            const twGainStage::Envelope e = t->gainStageComponent()->envelope();
            std::uintptr_t bits = 0;
            std::memcpy( &bits, &e.base, sizeof( bits ) < sizeof( e.base )
                                              ? sizeof( bits ) : sizeof( e.base ) );
            sig.push_back( bits );
            sig.push_back( e.muted ? 1u : 0u );
            sig.push_back( (std::uintptr_t) e.vol.get() );
            sig.push_back( (std::uintptr_t) e.mute.get() );
        }
        sig.push_back( 0xFFFFu );   // a member separator, so two shapes cannot alias
    }
    return sig;
}

std::uint64_t SLiveMonitor::liveOwnedRefusals()
{
    return audio::twPluginSlotProcessor::liveOwnedRefusals();
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
