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
    SStdMixer *rootMixer() const;
    void applyExclusion( const SLiveClosure &affected );  // flags -> wiring
    void publishPlan( const SLiveClosure &closure, std::uint64_t flipEpoch,
                      std::uint64_t flipEpochPrime );
    std::uint64_t rootEpoch() const;
    void retireClosureNodes( const SLiveClosure &closure );
    void setClosureOwned( const SLiveClosure &closure, bool owned );
    bool ensureInput( STrack *track );
    void closeInputIfUnused();

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
    bool ensureBridge( const QString &want );
    void closeBridge();
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
    int                                  bridgeHolds_ = 0;   // recording holds
    std::vector<std::shared_ptr<SLiveAudioInputSource> > sources_;
    std::unique_ptr<LiveGraphPump>       pump_;

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
