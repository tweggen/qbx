#include "app/shell/slivemonitor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QTimer>

#include "app/model/slink.h"
#include "app/model/sproject.h"
#include "app/model/sprojectprops.h"
#include "app/servicesui/soptions.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/objects/track/spluginchain.h"
#include "app/objects/track/spluginslot.h"
#include "app/objects/track/strack.h"
#include "app/shell/sapplication.h"
#include "app/shell/smidiinputhub.h"
#include "app/shell/smidioutpump.h"
#include "app/shell/sliveinputsource.h"
#include "app/shell/ssettings.h"
#include "tw/core/twlog.h"
#include "tw/devices/audio_input.h"
#include "tw/record/capture_bridge.h"
#include "tw/graph/twcomponent.h"
#include "tw/mix/twrewire.h"
#include "tw/mix/twtrackmix.h"
#include "tw/plugins/twpluginchain.h"
#include "tw/pages/tw_output_page.h"
#include "tw/devices/midi_out_scheduler.h"
#include "tw/playback/twliveeventclock.h"
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
    detachLiveEvents( current_ );
    detachLiveEvents( departing_ );
    setClosureOwned( current_, false );
    setClosureOwned( departing_, false );
    bridgeHolds_ = 0;
    closeBridge();
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
    // The track's own recording channels travel with the request: on ASIO the
    // device opens exactly what has been asked for (proposal 35 Phase 3), and
    // because that set is grow-only, arming tracks one at a time accumulates
    // the union without anyone having to compute it.
    return ensureBridge( want, track ? (std::uint64_t) track->getRecordingChannels() : 0 );
}

bool SLiveMonitor::ensureBridge( const QString &want, std::uint64_t channelMask )
{
    bool reopenForChannels = false;
    if( bridge_ && bridge_->isRunning() && inputDeviceId_ == want ) {
        const std::uint64_t union_ = openChannelMask_ | channelMask;
        if( channelMask == 0 || union_ == openChannelMask_ ) {
            // Already open and already covers every bit asked for — the
            // common case once a session is warm. Nothing to do.
            bridge_->setLiveEnabled( true );
            return true;
        }
        // A bit this bridge has never been asked for. `requestInputChannels`
        // alone would just DEFER on ASIO (its driver is running, and growing
        // its channel set needs disposeBuffers+createBuffers, which needs it
        // stopped) — and nothing forces that stop within one continuous
        // session, so the deferred grow could sit forever and the new
        // channel would silently never arrive. Reopen the bridge itself, with
        // the UNION of every mask asked for so far, so the selection takes
        // effect now rather than waiting for a start that may never come.
        reopenForChannels = true;
        channelMask = union_;
    }
    if( bridge_ ) {
        if( bridgeHolds_ > 0 ) {
            // A take is running on the device that is open. Reopening it —
            // whether for a device change or a wider channel set — would
            // throw the take away; keep what we have and say so.
            TW_LOGW( "shell", "[LIVE] input %s to '%s' deferred: a recording "
                              "holds '%s'",
                     reopenForChannels ? "channel mask change" : "device change",
                     want.toStdString().c_str(),
                     inputDeviceId_.toStdString().c_str() );
            return true;
        }
        closeBridge();
    }

    // ONE input pump (design D7). The bridge owns the device, its capture
    // thread and the ring drain; monitoring pops its live ring and a recording
    // opens a capture segment on it. Selected by SMARAGD_AUDIO_INPUT_BACKEND
    // ahead of the platform (L0), so a headless case replays a WAV through the
    // same capture thread and ring a device uses.
    const int rate = app_->get303aEnvironment() ? app_->get303aEnvironment()->getSRate()
                                                : 48000;
    audio::CaptureBridgeParams p;
    p.inputDeviceId = want.toStdString();
    p.targetRate    = (std::uint32_t) rate;
    // NO PAGES for a monitor-only session: they are the record of a RECORDING,
    // and growing them here would leak the user's RAM for audio nobody asked
    // to keep. beginCapture() opens a segment when a take starts.
    p.capturePages  = false;
    p.liveEnabled   = true;
    p.inputChannelMask = channelMask;

    std::unique_ptr<audio::CaptureBridge> br( new audio::CaptureBridge() );
    if( !br->start( p ) ) {
        TW_LOGW( "shell", "[LIVE] input device '%s' would not open: %s",
                 want.toStdString().c_str(), br->errorMessage() );
        return false;
    }
    bridge_          = std::move( br );
    inputDeviceId_   = want;
    openChannelMask_ = channelMask;
    TW_LOGI( "shell", "[LIVE] input open: device='%s' %u ch @ %u Hz -> %u Hz, "
                      "reported latency %u frames",
             want.toStdString().c_str(), bridge_->inputChannels(),
             bridge_->inputRate(), bridge_->targetRate(),
             bridge_->inputLatencyFrames() );
    return true;
}

void SLiveMonitor::closeBridge()
{
    if( !bridge_ ) return;
    bridge_->stop();
    bridge_.reset();
    inputDeviceId_.clear();
    openChannelMask_ = 0;
}

audio::CaptureBridge *SLiveMonitor::acquireBridge( const QString &deviceId )
{
    QString want = deviceId;
    if( want.isEmpty() ) want = SSettings::instance().audioInputDeviceId();
    if( want.isEmpty() ) want = QStringLiteral( "default" );
    if( !ensureBridge( want ) ) return nullptr;
    ++bridgeHolds_;
    // needsInput(), not empty(): a recording with monitoring off has nothing
    // popping the live ring, and neither has a lane that exists only because
    // the metronome is on. Leaving the push on would fill the ring once and
    // then count every frame of the take as an overrun.
    bridge_->setLiveEnabled( current_.needsInput() || departing_.needsInput() );
    return bridge_.get();
}

void SLiveMonitor::releaseBridge()
{
    if( bridgeHolds_ > 0 ) --bridgeHolds_;
    closeInputIfUnused();
}

void SLiveMonitor::closeInputIfUnused()
{
    // Again needsInput() rather than empty(): a lane that exists only because
    // the metronome is on must not hold the machine's microphone open.
    if( current_.needsInput() || departing_.needsInput() ) {
        if( bridge_ ) bridge_->setLiveEnabled( true );
        return;
    }
    if( bridgeHolds_ > 0 ) return;         // a take still owns the device
    closeBridge();
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

// --- live instruments (proposal 21 L2, design D2/D4/D8) ---------------------

void SLiveMonitor::attachLiveEvents( const SLiveClosure &closure )
{
    SMidiInputHub *hub = app_ ? app_->midiInputHub() : nullptr;
    if( !hub ) return;

    // THE CLOCK, once. It reads the ENGINE clock (the atomic the RT callback
    // stamps), and there is exactly one of those, so there is exactly one of
    // these. `playing` is republished here rather than polled because the
    // sources run on the PUMP and may not ask the app anything.
    const int rate = app_->get303aEnvironment()
                         ? app_->get303aEnvironment()->getSRate() : 48000;
    if( !eventClock_ ) {
        if( std::shared_ptr<twSpeaker> spk = app_->getSpeaker() )
            eventClock_ = std::make_shared<twLiveEventClock>( spk->engineClock(), rate );
    }
    if( eventClock_ ) {
        eventClock_->setSampleRate( rate );
        const bool playing = ( pendingPlaying_ >= 0 ) ? ( pendingPlaying_ != 0 )
                                                      : app_->isPlaying();
        eventClock_->setPlaying( playing );
    }

    // PRUNE FIRST. A consumer can stay in the closure (an audio input of its
    // own, or a second armed child) while the feed that put it there is gone;
    // keeping its source alive would keep draining a ring nobody asked for.
    for( auto it = midiLive_.begin(); it != midiLive_.end(); ) {
        const bool wanted =
            std::find_if( closure.midiFeeds.begin(), closure.midiFeeds.end(),
                          [&]( const SLiveMidiFeed &f ) {
                              return f.consumer == it->feed.consumer;
                          } ) != closure.midiFeeds.end();
        if( wanted ) { ++it; continue; }
        releaseLiveEntry( *it );
        it = midiLive_.erase( it );
    }

    for( const SLiveMidiFeed &feed : closure.midiFeeds ) {
        auto it = std::find_if( midiLive_.begin(), midiLive_.end(),
                                [&]( const MidiLive &m ) {
                                    return m.feed.consumer == feed.consumer;
                                } );
        if( it != midiLive_.end() ) {
            // Already live. A source is deliberately NOT rebuilt on a
            // republish: it holds the ring cursor and the HELD-NOTE TABLE, and
            // rebuilding it under a finger would drop the note being played.
            //
            // The THRU ROUTE IS NOT RE-EVALUATED EITHER, and that is a stated
            // limitation rather than an oversight: it is resolved once, at the
            // arm, so moving a track's `midiOutPort` while it is armed keeps
            // sending to the old port until the next arm. Re-routing it here
            // would mean panicking and re-opening a port under a held key,
            // which is a worse failure than the one it fixes.
            if( it->source ) it->source->setSampleRate( rate );
            continue;
        }

        SPluginSlot *slot = feed.consumer ? feed.consumer->instrumentSlot() : nullptr;
        const std::shared_ptr<audio::twPluginSlotProcessor> proc =
            slot ? slot->getProcessor() : nullptr;
        if( !proc ) continue;

        MidiLive live;
        live.feed   = feed;
        live.fanout = hub->fanoutFor( feed.port );
        if( !live.fanout ) continue;

        const std::uint16_t mask =
            ( feed.channel < 0 ) ? (std::uint16_t) 0xFFFF
                                 : (std::uint16_t)( 1u << feed.channel );
        live.sink = live.fanout->acquire(
            mask, feed.consumer->getSName().toStdString().c_str() );
        if( !live.sink ) continue;

        live.source = std::make_shared<audio::twLiveEventSource>( live.sink, rate );
        live.source->setClock( eventClock_ );
        // The input latency, in PROJECT frames. It is the USER's per-port
        // correction and nothing else: MidiInput has no latency to report -
        // no MIDI API this app hosts offers one - so the number a "play a
        // click, look at where it landed, type the difference" calibration
        // produces is the whole of it. POSITIVE means the byte arrived that
        // much AFTER the key went down, so the source subtracts it.
        const double offsetMs =
            SSettings::instance().midiInputOffsetMs( feed.port );
        live.source->setLatencyFrames(
            (offset_t) llround( offsetMs * rate / 1000.0 ) );
        // The one chase at live start: whatever is already held gets re-attacked
        // in the first block the processor renders.
        live.source->requestChase();

        // THE SECOND SOURCE (design D2). Never setEventSource.
        proc->setLiveEventSource( live.source );

        // MIDI-THRU (design D8). The ARMED track's port first - it is the one
        // being played - and the consumer's as the fallback, which is the
        // folder-drum-machine shape where the child has no port of its own.
        const bool ownPort = !feed.armed->getMidiOutPort().isEmpty();
        const QString thruPort = ownPort ? feed.armed->getMidiOutPort()
                                         : feed.consumer->getMidiOutPort();
        const int thruChannel = ownPort ? feed.armed->getMidiOutChannel()
                                        : feed.consumer->getMidiOutChannel();
        if( !thruPort.isEmpty() && app_->midiOutPump() ) {
            if( audio::MidiOutScheduler *sched =
                    app_->midiOutPump()->thruSchedulerFor( thruPort ) ) {
                if( live.fanout->setThru( sched, thruChannel ) )
                    live.thru = sched;
            }
        }

        TW_LOGI( "shell", "[LIVE] instrument armed: track='%s' port='%s' ch=%d "
                          "thru=%s",
                 feed.consumer->getSName().toStdString().c_str(),
                 feed.port.toStdString().c_str(), feed.channel,
                 live.thru ? thruPort.toStdString().c_str() : "off" );
        midiLive_.push_back( std::move( live ) );
    }
}

// The teardown half of ONE entry, in design D4's order. Called from the disarm
// path (before ownership is released) and from the prune in attachLiveEvents.
void SLiveMonitor::releaseLiveEntry( MidiLive &m )
{
    // 1. THE FLUSH. The source turns its held-note table into note-offs at
    //    offset 0 of whatever block the pump renders next. The hand-back
    //    GUARANTEES the rest: setLiveOwned(false) forgets continuity, so the
    //    freeze path's first render resets every instance and no voice can
    //    survive the disarm whatever the pump did or did not get to do.
    if( m.source ) m.source->requestAllNotesOff();

    // 2. Detach the SECOND SOURCE, before ownership goes (design D4).
    if( m.feed.consumer ) {
        if( SPluginSlot *slot = m.feed.consumer->instrumentSlot() ) {
            if( const std::shared_ptr<audio::twPluginSlotProcessor> proc =
                    slot->getProcessor() )
                proc->setLiveEventSource( nullptr );
        }
    }

    // 3. THRU stops, and the port PANICS: a key held when the user disarmed
    //    would otherwise be a stuck note on their hardware synth, which is the
    //    one failure mode a performer never forgives.
    if( m.fanout ) m.fanout->clearThru();
    if( m.thru )   m.thru->panic();
    if( m.fanout && m.sink ) m.fanout->release( m.sink );
    m.sink   = nullptr;
    m.fanout = nullptr;
    m.thru   = nullptr;
    m.source.reset();
}

void SLiveMonitor::detachLiveEvents( const SLiveClosure &leaving )
{
    for( auto it = midiLive_.begin(); it != midiLive_.end(); ) {
        if( !leaving.contains( it->feed.consumer ) ) { ++it; continue; }
        releaseLiveEntry( *it );
        it = midiLive_.erase( it );
    }
}

void SLiveMonitor::requestLiveChase()
{
    for( MidiLive &m : midiLive_ )
        if( m.source ) m.source->requestChase();
}

// --- the metronome and the count-in (proposal 21 L5) ------------------------

bool SLiveMonitor::metronomeEnabled() const
{
    SProject *p = app_ ? app_->getCurrentProject() : nullptr;
    return p && p->prop( SProjectProps::Metronome, false ).toBool();
}

std::uint64_t SLiveMonitor::ringFramesDelivered() const
{
    std::shared_ptr<twSpeaker> spk = app_ ? app_->getSpeaker()
                                          : std::shared_ptr<twSpeaker>();
    return spk ? spk->liveRing().framesDelivered() : 0ull;
}

// The click's SNAPSHOT, rebuilt only when it actually moved. A plan is
// republished for a fader move and for every transport edge, and the source
// carries two rendered click waveforms -- so comparing the config is what keeps
// a monitoring session from re-synthesising them thirty times a minute.
std::shared_ptr<twLiveInputSource> SLiveMonitor::ensureMetronome( bool want )
{
    if( !want ) { metronome_.reset(); return nullptr; }

    twMetronomeConfig cfg;
    if( SProject *p = app_ ? app_->getCurrentProject() : nullptr )
        cfg.tempo = p->tempoMap();          // THE tempo authority (37 D2)
    cfg.sampleRate = app_ && app_->get303aEnvironment()
                         ? app_->get303aEnvironment()->getSRate() : 48000;
    double level = SSettings::instance()
                       .value( SOpt::MetronomeLevel,
                               SOpt::def( SOpt::MetronomeLevel ) ).toDouble();
    if( !( level > 0.0 ) ) level = 0.0;
    if( level > 1.0 )      level = 1.0;
    // The downbeat (2 kHz square) is relative amplitude 1.0; every other beat
    // (1 kHz square) is 0.7 of it - twmetronome.h's tone spec.
    cfg.accentLevel = (float) level;
    cfg.beatLevel   = (float) ( level * 0.7 );
    if( countInActive_ ) {
        // THE COUNT-IN GRID IS ANCHORED AT THE RECORD POSITION and counts N
        // bars forward from it, over the stopped lane's ordinary virtual
        // counter. It is heard BEFORE the take because the transport has not
        // started yet -- the playhead does not move at all during a count-in --
        // so "N bars of click, then recording begins at the locator" holds
        // without any position ever going negative (see sliveplanbuilder.cpp).
        //
        // The RANGE is what makes the count exact: the pump renders one to two
        // blocks ahead, so a plain "stop clicking now" would always let the
        // downbeat past the end through (twmetronome.h).
        cfg.gridOrigin = countInAnchor_;
        cfg.rangeStart = countInAnchor_;
        cfg.rangeEnd   = countInAnchor_ + countInTotal_;
    }

    if( !metronome_ || metronomeCfg_ != cfg ) {
        metronomeCfg_ = cfg;
        metronome_    = std::make_shared<twMetronomeSource>( cfg );
    }
    return metronome_;
}

offset_t SLiveMonitor::barFrames() const
{
    SProject *p = app_ ? app_->getCurrentProject() : nullptr;
    if( !p ) return 0;
    const int rate = app_->get303aEnvironment() ? app_->get303aEnvironment()->getSRate()
                                                : 48000;
    const offset_t n = (offset_t) p->tempoMap().barFrames( rate ).floorToInt();
    return n > 0 ? n : 0;
}

void SLiveMonitor::beginCountIn( offset_t frames )
{
    if( frames <= 0 || !app_ ) return;
    countInTotal_  = frames;
    countInAnchor_ = app_->getGlobalLocatorPos();
    countInActive_ = true;
    // The click joins the plan and the lane opens; the stopped lane's virtual
    // counter runs FORWARD from the locator and the click's range covers
    // `[locator, locator + frames)`. THE BASELINE IS TAKEN AFTER refresh(),
    // because openLive() resets the ring - and its counters - when it opens the
    // device.
    refresh();
    if( pump_ ) pump_->requestReposition();
    countInBase_ = ringFramesDelivered();
}

void SLiveMonitor::muteCountIn()
{
    if( !countInActive_ || countInTotal_ == 0 ) return;
    countInTotal_ = 0;          // an EMPTY click range; see the header
    refresh();
}

void SLiveMonitor::endCountIn()
{
    if( !countInActive_ ) return;
    countInActive_ = false;
    countInTotal_  = 0;
    refresh();
}

offset_t SLiveMonitor::countInRemainingFrames() const
{
    if( !countInActive_ ) return 0;
    const std::uint64_t d = ringFramesDelivered();
    // A ring reset under us (a device re-open) restarts the count rather than
    // wrapping the subtraction into an eternity of remaining frames.
    if( d < countInBase_ ) countInBase_ = d;
    const offset_t done = (offset_t) ( d - countInBase_ );
    return countInTotal_ > done ? ( countInTotal_ - done ) : (offset_t) 0;
}

// --- the plan ---------------------------------------------------------------

void SLiveMonitor::publishPlan( const SLiveClosure &closure,
                                std::uint64_t flipEpoch,
                                std::uint64_t flipEpochPrime )
{
    std::shared_ptr<twSpeaker> spk = app_->getSpeaker();
    if( !spk ) return;

    // NO EXCLUSION, NO EPOCH GATE (proposal 21 L5). `flipEpoch` exists so the
    // RT does not sum a ring entry onto a root page that still CONTAINS the
    // armed track. A lane with no track members - a metronome-only one - nulled
    // no plug and bumped nothing, so there is nothing for the page to be too
    // old for, and passing an epoch here would gate the click off until an
    // unrelated re-freeze happened to land.
    if( closure.ordered.empty() ) { flipEpoch = 0; flipEpochPrime = 0; }

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

    // THE CLICK (proposal 21 L5). A synthetic plan track at the output; it owns
    // no STrack and live-owns nothing, so nothing above this line changes.
    p.metronome = ensureMetronome( closure.metronome );

    // One source object per source track, rebuilt with the plan: the scratch
    // and the channel map are sized HERE, on the main thread, so the pump's
    // pull() allocates nothing.
    sources_.clear();
    SLivePlanBuilder::SourceFn fn =
        [this, &closure, &p]( STrack *t ) -> std::shared_ptr<twLiveInputSource> {
            if( !ensureInput( t ) ) return nullptr;
            auto src = std::make_shared<SLiveAudioInputSource>(
                bridge_.get(), t->trackInputChannelMask(),
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
    // THE SPEAKER LEARNS THE MODE WITH THE PLAN, and BEFORE the pump starts
    // producing for it. The other order has a window in which the ring already
    // carries the whole master while the RT is still adding the frozen root
    // page -- the doubling, for as long as one callback.
    if( std::shared_ptr<twSpeaker> spk = app_ ? app_->getSpeaker()
                                              : std::shared_ptr<twSpeaker>() )
        spk->setLiveMasterClosure( !plan->masterLinear );
    pump_->setPlan( plan );
    pump_->start();
    publishedSignature_ = planSignature();
}

// --- the triggers -----------------------------------------------------------

bool SLiveMonitor::masterShapeRefusesMonitoring()
{
    SStdMixer *mixer = rootMixer();
    if( !mixer ) return false;

    // THE MASTER-SHAPE PRECONDITION (design D3).
    //
    // "root(unarmed) + ring" is exact only while the master is a unity sum
    // followed by an identity map. Since proposal 45 M3 the OTHER mode is
    // wired for one class of reason and still refused for the other, and the
    // split is `twMasterShape::fromMasterLane`:
    //
    //  * THE MASTER LANE doing something -- an insert, a fader, a mute, an
    //    automation lane -- is RENDERABLE. The plan builder gives the pump's
    //    master node that lane's inserts and gain envelope (AC3.2), the pump
    //    reads the unarmed tracks as per-track `frozenInputs`, and twSpeaker
    //    stops adding the frozen root page (setLiveMasterClosure). The
    //    arrangement is heard once, through the master, from the ring.
    //
    //  * THE MIXER's own sum -- a non-unity input level, a non-identity
    //    channel map, a width disagreement -- is still REFUSED, because the
    //    pump has NO per-input level anywhere: a plan built for that shape
    //    would silently drop it. One log line naming the reason, and the
    //    arrangement keeps playing untouched.
    {
        const twlive::twMasterShape shape = twlive::checkMasterShape(
            mixer->masterMixComponent().get(),
            mixer->masterRewireComponent().get(),
            (idx_t) app_->masterChannels(),
            sliveplan::masterChainStateOf( mixer ) );
        // PROPOSAL 45 M3 / D4b: a LANE-CAUSED non-linear master is renderable
        // as a CLOSURE -- the pump renders the whole arrangement through the
        // master lane's own chain and gain -- while a MIXER-CAUSED one (a
        // non-unity sum, a non-identity map) is not, and is still refused.
        // Hence the two-part condition rather than `!shape.linear()`.
        //
        // GETTING HERE COST TWO ROUNDS OF DIAGNOSIS AND THE FIRST ROUND'S
        // FINDINGS WERE ALL WRONG. The write-up is in
        // plan/proposed/45_SYSTEM_LANES.md, M3; what a reader needs here is
        // the one structural fact it turned up:
        //
        //   AC3.1 CANNOT BE "STOP PULLING THE FROZEN LANE". twSpeaker
        //   publishes the transport position from engine->currentPosition()
        //   AFTER the frozen pull, so suppressing the pull stops the playhead
        //   -- measured, the demand position stayed at 0 for a whole 5.2 s run
        //   and the unarmed track went silent. The suppression drops the SUM
        //   and keeps the pull; see the comment at twspeaker.cc's
        //   `frozenPlaying`.
        //
        // Measured with that in place, over [168000, 216000) -- past the
        // priming lag AND inside the material, both of which a first attempt
        // got wrong -- with an unarmed track at -12 dB carrying
        // test_autosaw.wav and an armed, monitored track on the paced file
        // input through a 0.25 gain, master insert 2.0x:
        //
        //   ring alone, Closure                0.164143   closed form 0.164142
        //   root + ring, LINEAR                0.101661   closed form 0.100505
        //   root + ring, Closure               0.201039   closed form 0.201010
        //   root at -6 dB + ring, Closure      0.283832   closed form 0.283790
        //
        // and the root-presence discriminator (raise the unarmed track 6 dB,
        // watch the output) reads 1.4118 against a closed form of 1.4118. It
        // read 1.0046 -- the arrangement absent -- before the AC3.1 rebuild.
        //
        // HEADROOM IS PART OF THE MEASUREMENT, not housekeeping: an earlier
        // probe put a 2.0x master over full-scale sources and clipped the
        // 16-bit capture at peak exactly 1.0000, which read as an
        // unexplained ~11 % shortfall and was very nearly written up as one.
        if( !shape.linear() && !shape.fromMasterLane ) {
            if( lastRefusal_.isEmpty() ) {
                lastRefusal_ = QStringLiteral(
                    "the master is not a unity sum with an identity map (%1); "
                    "live monitoring is off" ).arg( QString::fromUtf8( shape.reason ) );
                TW_LOGW( "shell", "[LIVE] %s", lastRefusal_.toStdString().c_str() );
            }
            return true;
        }
    }
    return false;
}

void SLiveMonitor::refresh()
{
    if( !app_ || suspendedForRender_ ) return;
    SStdMixer *mixer = rootMixer();
    if( !mixer ) return;

    const bool playing = ( pendingPlaying_ >= 0 ) ? ( pendingPlaying_ != 0 )
                                                  : app_->isPlaying();
    SLiveClosure want = sliveplan::computeClosure(
        mixer, playing, app_->isRecordingActive(), inertlyArmed_ );
    // A LIVE LANE EXISTS IFF armed u monitor u metronome (design D9). The
    // metronome owns no track, so it joins as a FLAG and leaves the whole
    // arm/disarm protocol below untouched.
    const bool clickWhileRecording = SSettings::instance()
        .value( SOpt::ClickWhileRecording,
                SOpt::def( SOpt::ClickWhileRecording ) ).toBool();
    want.metronome = sliveplan::metronomeWanted(
        metronomeEnabled(), playing, app_->isRecordingActive(), countInActive_,
        clickWhileRecording );

    const bool sameSet = ( want.ordered == current_.ordered )
                         && ( want.sources == current_.sources )
                         && ( want.midiFeeds == current_.midiFeeds )
                         && ( want.metronome == current_.metronome );
    if( sameSet ) {
        // Nothing structural moved: only the transport, the fader or an insert
        // did. Rebuild and republish -- a plan is a SNAPSHOT, so a fader move
        // on a closure member is only heard once a new one is built (design
        // section 3's rebuild triggers).
        //
        // The live sources are re-offered too, and it is NOT a no-op: the
        // transport half of the clock lives there, and a Play/Stop with the
        // same closure is exactly the case where the mapping changes and the
        // set does not.
        // empty() accounts for the metronome, so a click-only lane republishes
        // here too - which is what a tempo edit while playing needs.
        if( !current_.empty() ) {
            // THE SHAPE IS RE-ASKED HERE, and this is the whole point of the
            // extraction: an insert, a fader, a mute or an automation lane on
            // the master moves NO membership, so this is the ONLY route a
            // master change reaches while a track is already monitoring - and
            // for a track whose monitor mode is ON it is also the route ARMING
            // takes, because such a track was in the live set before it was
            // armed.
            if( masterShapeRefusesMonitoring() ) {
                tearDownLiveForRefusal();
                return;
            }
            attachLiveEvents( current_ );
            publishPlan( current_, rootEpoch(), 0 );
        }
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
        // THE LIVE EVENT SOURCE GOES FIRST (design D4's disarm order): the
        // all-notes-off flush is asked for while the processor still HAS the
        // source, and the source is detached before ownership - never after,
        // because setLiveOwned(false) drops it anyway and the flush would then
        // have nowhere to land.
        detachLiveEvents( leaving );
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
        // A METRONOME-ONLY LANE LEAVES THROUGH NO DISARM PATH. It live-owned
        // nothing and nulled no plug, so `leaving` is empty and finishDisarm()
        // will never run - which before L5 could not happen, because the only
        // way to reach an empty set was for a track to have left. Without this
        // the pump would keep clicking off the old plan forever.
        if( leaving.ordered.empty() && departing_.empty() ) {
            stopPump();
            ensureMetronome( false );
            closeInputIfUnused();
            if( liveOpened_ ) {
                if( std::shared_ptr<twSpeaker> spk = app_->getSpeaker() ) {
                    // Clear the mode BEFORE closing: a stale "closure" left on
                    // the speaker would suppress the frozen lane for a project
                    // that is merely playing, which is silence.
                    spk->setLiveMasterClosure( false );
                    spk->closeLive();
                }
                liveOpened_ = false;
            }
        }
        return;
    }

    // THE MASTER-SHAPE PRECONDITION, asked on this route AND on the `sameSet`
    // fast path above (see masterShapeRefusesMonitoring's declaration for what
    // asking on only one of them cost).
    if( masterShapeRefusesMonitoring() ) {
        // Nothing is armed on this path, but a PREVIOUS pass may have left
        // live sources attached (the master can only stop being linear while
        // something is already monitoring). Dropping the closure without
        // dropping them would leave a ring being drained for a lane nobody
        // renders.
        detachLiveEvents( current_ );
        current_ = SLiveClosure();
        return;
    }

    if( !arriving.ordered.empty() ) {
        // 1. drain, 2. own, 3. wire + bump, 4. read the epoch.
        retireClosureNodes( arriving );
        setClosureOwned( arriving, true );
        // ...and only THEN the second event source (design D4): a live source
        // installed on a processor the freeze path still owns would be
        // collected by a freeze worker, which is the one reader it may not
        // have.
        for( STrack *t : arriving.ordered ) t->setLiveOwnedLane( true );
        applyExclusion( arriving );
    }
    // ...and only THEN the second event source (design D4): a live source
    // installed on a processor the freeze path still owns would be collected
    // by a freeze worker, which is the one reader it may not have. Outside the
    // `arriving` guard on purpose - a feed can change (a different port, a
    // different channel, a new armed child) while the closure does not.
    attachLiveEvents( want );
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
            detachLiveEvents( current_ );
            setClosureOwned( current_, false );
            applyExclusion( current_ );
            current_ = SLiveClosure();
            return;
        }
    }

    // 7. publish, then one explicit reposition.
    publishPlan( current_, flipEpoch, 0 );
    if( pump_ ) pump_->requestReposition();
    requestLiveChase();
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
        requestLiveChase();
    }
    detachLiveEvents( gone );          // idempotent
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
    requestLiveChase();
}

void SLiveMonitor::seeked()
{
    if( !pump_ ) return;
    // A seek changes the position the pump must render at, and while STOPPED it
    // also changes the anchor and the automation hold -- both live in the plan.
    if( !current_.empty() ) publishPlan( current_, rootEpoch(), 0 );
    pump_->requestReposition();
    requestLiveChase();
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
    detachLiveEvents( current_ );
    setClosureOwned( current_, false );
    for( STrack *t : current_.ordered ) t->setLiveOwnedLane( false );
    SLiveClosure was = current_;
    current_ = SLiveClosure();
    // Only when something was WIRED. A metronome-only lane nulled no plug and
    // live-owned nothing, so there is nothing to undo - and applyExclusion's
    // empty-set fallback would stale the master chain for a render that is
    // about to freeze it, for no reason at all.
    if( !was.ordered.empty() ) applyExclusion( was );
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
    // THE REFUSAL IS RE-ASKED HERE, not only on arm/disarm/transport. An edit
    // that changes the signature can also change whether monitoring is LEGAL
    // at all -- a mixer-caused non-linear master (a non-unity input level, a
    // non-identity channel map) has no representable plan, so republishing one
    // would hand the pump a shape it silently mis-renders.
    //
    // NOT GATED, and it is the one half of this fix that is not: no verb in
    // this repo can set a master input level or a channel map, so the
    // mixer-caused branch is unreachable from a script. What the new case
    // master_insert_while_monitoring bites is the OTHER half, the signature
    // below.
    if( masterShapeRefusesMonitoring() ) { tearDownLiveForRefusal(); return; }
    publishPlan( current_, 0, 0 );
}

void SLiveMonitor::tearDownLiveForRefusal()
{
    detachLiveEvents( current_ );
    current_ = SLiveClosure();
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

// ONE SPELLING OF "what about this track can make a published plan stale",
// used for a closure member and for the MASTER LANE alike. It was inlined in
// planSignature() and the master lane simply was not asked; a second, hand-
// written copy for the master would be the same bug waiting on a third
// property being added to only one of them.
static void appendTrackSignature( std::vector<std::uintptr_t> &sig, STrack *t )
{
    if( !t ) { sig.push_back( 0u ); return; }
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

std::vector<std::uintptr_t> SLiveMonitor::planSignature() const
{
    std::vector<std::uintptr_t> sig;
    for( STrack *t : current_.ordered )
        appendTrackSignature( sig, t );

    // THE MASTER LANE, WHICH IS NOT A CLOSURE MEMBER AND WAS THEREFORE INVISIBLE
    // HERE. D2 keeps the master lane out of childLinks(), so it is in no
    // closure and the loop above never reaches it -- which meant an insert, a
    // fader move, a mute or an automation lane on the MASTER changed nothing
    // in this signature and pumpEdits() never fired. The live plan then kept
    // whatever master shape it was built with for the whole life of the arm.
    //
    // Under the LINEAR split that is not cosmetic: the freeze path applies the
    // new master insert to the frozen root page while the ring bypasses it, and
    // the RT adds the two together. The halves stop belonging to the same
    // signal -- the mismatch D4a's shape check exists to prevent, undetected
    // because nothing re-asked. Gate: master_insert_while_monitoring.
    sig.push_back( 0xEEEEu );
    if( SStdMixer *mixer = rootMixer() )
        appendTrackSignature( sig, mixer->masterLane() );
    // THE CLICK, so a TEMPO or TIME-SIGNATURE edit republishes (design section
    // 3's rebuild triggers). Asked rather than wired, exactly like the fader:
    // `set-tempo` re-derives every beats-timebase link in the project and would
    // otherwise have to know about the live lane as well.
    sig.push_back( current_.metronome ? 1u : 0u );
    if( current_.metronome ) {
        if( SProject *p = app_ ? app_->getCurrentProject() : nullptr ) {
            sig.push_back( (std::uintptr_t) p->tempoMap().usPerQuarter() );
            sig.push_back( (std::uintptr_t) p->tempoMap().numerator() );
            sig.push_back( (std::uintptr_t) p->tempoMap().denominator() );
        }
        sig.push_back( (std::uintptr_t) llround(
            1000.0 * SSettings::instance()
                         .value( SOpt::MetronomeLevel,
                                 SOpt::def( SOpt::MetronomeLevel ) ).toDouble() ) );
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
    if( bridge_ )
        s += QStringLiteral( " input=%1 inCh=%2 inLat=%3" )
                 .arg( inputDeviceId_ )
                 .arg( bridge_->inputChannels() )
                 .arg( bridge_->inputLatencyFrames() );
    if( pump_ )
        s += QStringLiteral( " blocks=%1 repositions=%2 misses=%3 shortfalls=%4" )
                 .arg( pump_->blocks() ).arg( pump_->repositions() )
                 .arg( pump_->frozenInputMisses() ).arg( pump_->inputShortfalls() );
    if( !midiLive_.empty() ) {
        s += QStringLiteral( " midi=%1" ).arg( midiLive_.size() );
        for( const MidiLive &m : midiLive_ ) {
            s += QStringLiteral( " [%1<-%2:%3 held=%4 late=%5 thru=%6]" )
                     .arg( m.feed.consumer ? m.feed.consumer->getSName()
                                           : QStringLiteral( "?" ) )
                     .arg( m.feed.port )
                     .arg( m.feed.channel < 0 ? QStringLiteral( "any" )
                                              : QString::number( m.feed.channel ) )
                     .arg( m.source ? (qulonglong) m.source->heldNotes() : 0ull )
                     .arg( m.source ? (qulonglong) m.source->lateClamped() : 0ull )
                     .arg( m.thru ? QStringLiteral( "on" ) : QStringLiteral( "off" ) );
        }
    }
    if( current_.metronome )
        s += QStringLiteral( " metronome=on clicks=%1" )
                 .arg( metronome_ ? (qulonglong) metronome_->clicksEmitted() : 0ull );
    else
        s += QStringLiteral( " metronome=off" );
    if( countInActive_ )
        s += QStringLiteral( " countIn=%1/%2" )
                 .arg( (qlonglong) countInRemainingFrames() )
                 .arg( (qlonglong) countInTotal_ );
    if( !lastRefusal_.isEmpty() ) s += QStringLiteral( " refused='%1'" ).arg( lastRefusal_ );
    return s;
}
