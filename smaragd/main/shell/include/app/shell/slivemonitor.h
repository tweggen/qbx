#ifndef _SLIVEMONITOR_H_
#define _SLIVEMONITOR_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <QObject>
#include <QString>

#include "app/shell/sliveplanbuilder.h"
#include "tw/core/twtypes.h"
// The fan-out's Sink is a NESTED type, so it cannot be forward-declared; and
// the live event source is held by shared_ptr and destroyed here. Both are
// tw/devices, which app/shell already depends on.
#include "tw/devices/midi_in_fanout.h"
#include "tw/devices/twliveeventsource.h"
// The metronome CONFIG is held by value (the comparison is what decides whether
// a republish needs a new source), so it cannot be forward-declared.
#include "tw/playback/twmetronome.h"

class QTimer;
class SApplication;
class SStdMixer;
class STrack;
class LiveGraphPump;

namespace audio {
class AudioInput;
class CaptureBridge;
}

class SLiveAudioInputSource;
class twLiveEventClock;

namespace audio {
class MidiOutScheduler;
}

/**
 * SLiveMonitor - the APP half of the live lane (proposal 21 L1b, design
 * D3/D4/D5/D9). One per SApplication, a sibling of SMidiOutPump and
 * SAutomationRecorder, and like them it is constructed with the app and
 * destroyed FIRST, on the main thread, while the log sink is alive: it owns a
 * std::thread (the pump) whose join must not happen during static teardown.
 *
 * IT OWNS THE ORDERING, and the ordering is the whole of design D3. A verb
 * never does this work; it calls SAppContext::liveLanesChanged() and the
 * sequence happens here, once, with the pump and the speaker in reach.
 *
 * ARM (main thread, in exactly this order):
 *   1. retireComponentNodes(closure)  - no in-flight freeze holds the chain
 *   2. setLiveOwned(true) on every processor in the closure
 *   3. wire the exclusion (isLiveOwnedLane + applyAudibility) and bump the
 *      render-chain epoch
 *   4. flipEpoch = the ROOT REWIRE's contentEpochNow(), read right after (3)
 *   5. build and publish the plan
 *   6. openLive() if this is the first lane, then requestReposition()
 *
 * DISARM is the mirror (design D2's "the plan's last block carries flipEpoch'"):
 *   1. retireComponentNodes(closure)
 *   2. un-wire, bump  -> the frozen sum wants the track back
 *   3. flipEpochPrime = the root rewire's contentEpochNow()
 *   4. publish the TAIL plan - the departing members are still rendered - with
 *      flipEpochPrime, so the RT keeps summing the ring while the page it
 *      serves still LACKS the track and stops the moment the re-summed one
 *      lands
 *   5. after kDisarmTailMs: publish the plan without them (or stop the pump),
 *      then setLiveOwned(false) + forgetContinuity(), then bump AGAIN so any
 *      page frozen during the tail (which would have been silent for that
 *      track) is stale rather than adopted, then closeLive() if nothing is live
 *
 * Step 5's window is the one place where a freeze worker and the pump can both
 * reach a processor. That is not an oversight: zero overlap and zero gap cannot
 * both be had, the design chose zero gap (the epoch gate plus a crossfade), and
 * the ownership guard exists precisely so the overlap costs a counted silence
 * (`liveOwnedRefusals`) rather than a corrupted voice. The second bump is what
 * makes it self-healing.
 *
 * THREADING. Everything here is the main thread. The pump is the only thread
 * that touches the ring, and nothing in this class does - design section 4.
 */
class SLiveMonitor : public QObject
{
    Q_OBJECT
public:
    /// How long the disarm tail keeps rendering the departing closure, in ms.
    /// Long enough for a re-summed root page to land, short enough that the
    /// ownership overlap above is measured in blocks rather than seconds.
    static constexpr int kDisarmTailMs = 250;
    /// The re-rooted horizon demand tick (design D3). A sibling of the
    /// readahead's own 20 ms tick, and for the same reason: the pump never
    /// demands, so somebody on the main thread has to.
    static constexpr int kDemandTickMs = 40;
    /// How far ahead the re-rooted demands reach, in pages.
    static constexpr int kDemandPages = 2;

    explicit SLiveMonitor( SApplication *app );
    ~SLiveMonitor() override;

    /// THE one plan-rebuild trigger (design section 3). Idempotent and cheap
    /// when nothing changed: the closure is recomputed and compared, and only
    /// a difference costs a rewire.
    void refresh();

    /// Transport start / stop / seek / loop wrap: one explicit reposition
    /// (design D2). Also rebuilds, because the transport decides the feed
    /// policy and the automation hold.
    void transportChanged();
    /**
     * The transport is ABOUT to become `playing`, before twSpeaker::startOutput
     * / stopOutput runs.
     *
     * The order is load-bearing, not cosmetic. `setPlaybackRunning` starts the
     * readahead FIRST and flips `isPlaying_` after, so a rebuild driven by the
     * flag alone would leave a track that monitor Auto is about to release
     * still live-owned while the readahead was already freezing its chain -
     * measured as six `liveOwnedRefusals` and six silent freezes. Releasing it
     * BEFORE the frozen lane starts is what makes the Auto hand-over clean.
     */
    void transportAboutToChange( bool playing );
    void seeked();

    /// A render suspends every live lane for its duration and comes back as a
    /// FRESH arm, never a resume (design D4). Export ignores the split.
    void suspendForRender();
    void resumeAfterRender();

    /// A project was loaded: every track that arrived armed is INERT until the
    /// user arms it in this session (design D9).
    void projectChanged();

    // --- the metronome and the count-in (proposal 21 L5) -------------------

    /**
     * Begin a COUNT-IN of `frames` project frames, ending at the LOCATOR.
     *
     * The reading taken (see `main/shell/CONTRACT.md`): the count-in is BEFORE
     * the record position, Cubase / Logic / REAPER style. THE PLAYHEAD DOES NOT
     * MOVE - the click plays for N bars on the stopped lane while the transport
     * sits at the locator, and recording then begins AT THE LOCATOR, so the
     * placed clip lands exactly where it would have without a count-in and the
     * capture holds N bars of clicks before it.
     *
     * The click grid is anchored AT the record position and counts forward,
     * which is why no position here is ever negative; running the virtual
     * counter backwards from `locator - frames` was the first design and made a
     * count-in at bar 1 silent (sliveplanbuilder.cpp says why).
     *
     * It is measured in FRAMES THE RT HAS ACTUALLY BEEN HANDED
     * (`twLiveMixRing::framesDelivered`), not in wall clock: while stopped
     * there is no engine clock at all, and a QTimer would measure the Windows
     * scheduler (15.6 ms of granularity) against a beat grid the same case
     * asserts to the frame.
     */
    void beginCountIn( offset_t frames );
    /**
     * The count-in has counted. THE CLICK STOPS HERE; THE LANE DOES NOT.
     *
     * Both halves are load-bearing and both were paid for by a failing gate.
     *
     * The click has to stop BEFORE the transport starts, because the count-in
     * grid lives in the ARRANGEMENT's position domain: the transport start
     * repositions the pump back to the locator, and the pump would then render
     * the very first beat of the count-in a SECOND time. Measured as a fifth,
     * accented click after a one-bar count-in.
     *
     * The lane has to survive, because dropping the last live lane calls
     * `twSpeaker::closeLive()`, which CLOSES the device while the frozen lane
     * is still stopped - and the transport start would then re-open it, which
     * clears the capture backend's recording and takes the whole count-in with
     * it. Keeping the lane up means the transport start ATTACHES the frozen
     * lane to a device that is already running (design D5), and nothing
     * restarts.
     *
     * So the range is closed to zero length and the source keeps its seat.
     */
    void muteCountIn();
    /// End it (the click leaves the plan unless the metronome is on and the
    /// transport is rolling). Idempotent.
    void endCountIn();
    bool     countInActive() const { return countInActive_; }
    /// Frames still to go; 0 once the count-in is over or was never begun.
    offset_t countInRemainingFrames() const;
    /// Frames per bar at the project's current tempo and time signature, from
    /// THE tempo authority. 0 when there is no project.
    offset_t barFrames() const;

    bool active() const;
    /// Is this track being rendered by the pump right now?
    bool isLive( const STrack *track ) const;
    /// Peak of what the input delivered, 0..1, WITHOUT clearing it - the
    /// assertion's view, which must not depend on who read it last.
    double inputPeak( const STrack *track ) const;
    /// The same peak, CLEARED - the meter's view. The pre-FX input meter
    /// (design D9) needs a since-the-last-tick reading or the bar latches at
    /// the loudest thing that ever happened.
    double takeInputPeak( const STrack *track );
    /// Freeze-path renders that arrived at a LIVE-OWNED processor and were
    /// answered with silence (design D4). Process-wide. Surfaced here rather
    /// than read straight off `twPluginSlotProcessor` because `app/testkit`
    /// may not include `tw/plugins`, and one accessor beats widening a
    /// module boundary for a counter.
    static std::uint64_t liveOwnedRefusals();
    /// How many openLive() calls the speaker refused for a rate mismatch.
    std::uint64_t rateRefusals() const;
    /// Human-readable state for the head tooltip and the testkit.
    QString describe() const;

private slots:
    void finishDisarm();
    void pumpDemands();
    void pumpEdits();

private:
    // One live event source per CONSUMING track (proposal 21 L2). Keyed by the
    // consumer rather than by the armed track because that is what the
    // processor hangs off: two armed children bubbling into one folder
    // instrument share ONE source and therefore one ring, which is also the
    // only shape an SPSC ring allows.
    struct MidiLive {
        SLiveMidiFeed                              feed;
        audio::MidiInFanout                       *fanout = nullptr;
        audio::MidiInFanout::Sink                 *sink   = nullptr;
        std::shared_ptr<audio::twLiveEventSource>  source;
        audio::MidiOutScheduler                   *thru   = nullptr;
    };

    SStdMixer *rootMixer() const;
    void applyExclusion( const SLiveClosure &affected );  // flags -> wiring
    void publishPlan( const SLiveClosure &closure, std::uint64_t flipEpoch,
                      std::uint64_t flipEpochPrime );
    std::uint64_t rootEpoch() const;
    void retireClosureNodes( const SLiveClosure &closure );
    void setClosureOwned( const SLiveClosure &closure, bool owned );
    bool ensureInput( STrack *track );
    void closeInputIfUnused();
    /**
     * THE MASTER-SHAPE PRECONDITION (design D3 / D4a), asked on EVERY route to
     * publishPlan() and not just the one that re-wires.
     *
     * It is a method rather than an inline block because `refresh()` reaches
     * publishPlan() TWICE: through the full arm/disarm path, and through the
     * `sameSet` fast path taken whenever the closure MEMBERSHIP did not move.
     * The check used to live only on the first, and a track whose monitor mode
     * is ON is in the live set BEFORE it is armed - so arming it never changed
     * the membership, the fast path was taken, and the master shape was never
     * consulted at all. Under proposal 45 M2, where every non-linear master was
     * refused, that meant a limiter dropped on the master while a track was
     * monitoring kept the LINEAR split silently: the RT went on adding a frozen
     * root page the master had processed to a ring that had bypassed it, which
     * is precisely the doubling D4a exists to prevent, with no log line because
     * nothing had looked.
     *
     * Returns true when monitoring must stand down; logs the reason ONCE.
     */
    bool masterShapeRefusesMonitoring();
    /**
     * Stand the live lane down because the master shape now refuses it.
     * Extracted so refresh()'s fast path and the pumpEdits() tick cannot
     * drift: they are the two routes a master change reaches, and a teardown
     * spelled twice is a teardown that will one day be spelled differently.
     */
    void tearDownLiveForRefusal();
    /// AC3.1 / D4b: the master lane's own processors are live-owned while a
    /// Closure plan is live, so the pump is their only legal renderer and the
    /// existing ownership guard counts anybody else.
    void setMasterLaneOwned( SStdMixer *mixer, bool owned );
    /// Every route OUT of a Closure: release the master lane's processors,
    /// clear the speaker's flag and put the frozen lane's root back above the
    /// master chain. See the definition for why the root, unlike the flag,
    /// cannot be left stale.
    void endMasterClosure();
    /// The click, rebuilt only when its snapshot actually changed (a plan is
    /// republished every time a fader moves; the source holds click waveforms).
    std::shared_ptr<twLiveInputSource> ensureMetronome( bool want );
    /// Is the project's metronome switch on?
    bool metronomeEnabled() const;
    std::uint64_t ringFramesDelivered() const;

public:
    /**
     * THE APP'S ONE INPUT PUMP (proposal 21 L3b, design D7).
     *
     * `SLiveMonitor` OWNS the `CaptureBridge` because it already owned the
     * input device: monitoring and recording must read ONE device, and the
     * bridge is the object that drains its ring. Monitoring pops the bridge's
     * live ring (`SLiveAudioInputSource` -> `pullLive`); recording opens a
     * capture SEGMENT on the same bridge (`beginCapture`), which is why a
     * record start while monitoring does not gap the monitored signal.
     *
     * `SAudioRecorder` BORROWS it: it never constructs or destroys one. The
     * hold below is what stops `closeInputIfUnused()` from pulling the device
     * out from under a take when the last lane disarms mid-recording.
     */
    audio::CaptureBridge *acquireBridge( const QString &deviceId );
    void releaseBridge();
    audio::CaptureBridge *bridge() const { return bridge_.get(); }
    /// The device the bridge is open on ("" when closed).
    QString inputDeviceId() const { return inputDeviceId_; }

private:
    // `channelMask` is the ARMING TRACK's recording channels (bit n == input
    // n). It is passed down rather than computed from a project walk
    // because the device's channel set is GROW-ONLY: each arm adds its own
    // channels, and the union falls out without anyone maintaining it.
    bool ensureBridge( const QString &want, std::uint64_t channelMask = 0 );
    void closeBridge();
    // --- live INSTRUMENTS (proposal 21 L2, design D2/D4/D8) ----------------
    //
    // THE ORDER IS THE PROTOCOL, and it is the mirror image of the audio one:
    //
    //   arm     retire -> setLiveOwned(true) -> attachLiveEvents(...)
    //           -> wire the exclusion -> read the epoch -> publish
    //   disarm  detachLiveEvents(...)  [all-notes-off + setLiveEventSource(0)]
    //           -> setLiveOwned(false) (which forgets continuity)
    //           -> un-wire -> read the epoch' -> publish the tail
    //
    // `setLiveEventSource` is deliberately NOT a `setEventSource` swap and NOT
    // a member of `STrack::eventFeed()` (design D2): the feed is re-applied by
    // `STrack::syncInstrumentSlot()` from adopt / insert / remove and would
    // silently overwrite a live source, and it is ALSO read by SMidiOutPump and
    // `assert-midi-events`, while a ring-draining collect has exactly one legal
    // reader.
    void attachLiveEvents( const SLiveClosure &closure );
    void detachLiveEvents( const SLiveClosure &leaving );
    void releaseLiveEntry( MidiLive &m );
    /// Ask every live source to re-attack what the performer is holding. Runs
    /// wherever the pump is asked to reposition - a reposition resets the
    /// instrument's voices, and a held key must survive it (design D4).
    void requestLiveChase();
    void ensurePump();
    void stopPump();
    /**
     * What the plan would contain if it were rebuilt right now, as a cheap
     * comparable value.
     *
     * Design section 3 lists "fader / insert / bypass / reorder edits on a
     * closure member" among the plan-rebuild triggers, and a plan is a
     * SNAPSHOT: a fader moved while monitoring is inaudible until a new one is
     * built. Wiring a signal from every one of those edit paths into here
     * would be four couplings for one question, and the question is cheap to
     * ask: the processors in slot order and the gain envelope, per member. The
     * demand tick asks it and republishes only on a difference, so a monitoring
     * session that nobody is editing costs one vector compare every 40 ms.
     */
    std::vector<std::uintptr_t> planSignature() const;

    SApplication *app_ = nullptr;

    // The live set as it stands, and the members whose exclusion wiring is
    // currently applied. They differ only during the disarm tail.
    SLiveClosure current_;
    SLiveClosure departing_;

    std::unique_ptr<audio::CaptureBridge> bridge_;
    QString                              inputDeviceId_;
    // The union of every channel bit any arm has asked for on the currently
    // open bridge. `AsioDevice::requestInputChannels()` defers a grow to "the
    // next start" while the driver is running, and nothing forces that start
    // within one continuous monitoring/recording session — so `ensureBridge`
    // tracks this to notice when a NEW bit is asked for and reopens the
    // bridge itself rather than silently waiting for a start that may never
    // come. 0 while no bridge is open.
    std::uint64_t                        openChannelMask_ = 0;
    int                                  bridgeHolds_ = 0;   // recording holds
    std::vector<std::shared_ptr<SLiveAudioInputSource> > sources_;
    std::unique_ptr<LiveGraphPump>       pump_;

    // THE CLICK (proposal 21 L5). Owned here for the same reason the audio
    // input sources are: the plan holds it while the pump renders, and the
    // monitor is the one object that knows when the plan is republished.
    std::shared_ptr<twMetronomeSource>   metronome_;
    twMetronomeConfig                    metronomeCfg_;
    bool                                 countInActive_ = false;
    offset_t                             countInTotal_  = 0;
    offset_t                             countInAnchor_ = 0;
    mutable std::uint64_t                countInBase_   = 0;

    std::vector<MidiLive>                midiLive_;
    // The host-time -> project-frame mapping the live sources share. One per
    // monitor: it reads the ENGINE clock, which there is exactly one of.
    std::shared_ptr<twLiveEventClock>    eventClock_;

    QTimer *disarmTimer_ = nullptr;
    QTimer *demandTimer_ = nullptr;
    std::vector<std::shared_ptr<void> >  demands_;   // one handle per frozen root

    // Tracks whose ArmedForRecording came out of a file (design D9).
    std::vector<const STrack *> inertlyArmed_;

    // >= 0 while a transport edge is in flight: the state the closure must
    // be computed for, which is not yet the app's own flag.
    int  pendingPlaying_ = -1;
    bool suspendedForRender_ = false;
    SLiveClosure suspended_;
    bool liveOpened_ = false;
    QString lastRefusal_;
    std::vector<std::uintptr_t> publishedSignature_;
};

#endif // _SLIVEMONITOR_H_
