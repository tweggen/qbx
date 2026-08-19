
#ifndef _SAPPLICATION_H
#define _SAPPLICATION_H

#include <memory>
#include <atomic>

#include "tw/graph/tw303aenv.h"
#include "tw/graph/twcomponent.h"
#include "tw/render/render_session.h"
#include "tw/playback/playback_context.h"
#include "tw/metering/tw_level_probe.h"
#include "app/model/sappcontext.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QString>
//#include <qptrlist.h>

class tw303aEnvironment;
class twSpeaker;
class twWhiteNoise;
class SObject;
class SLink;
class SProject;
class SActionHistory;
class SAction;
class SMidiOutPump;
class SMidiInputHub;
class SAutomationRecorder;
class SLiveMonitor;
class SAudioRecorder;
class SMidiRecorder;
class SMediaAccountManager;
class QTimer;

typedef QList<SLink*> SSelectionList;

/**
 * This object glues all wires together.
 *
 * Besides other things, it contains various stuff that should
 * not be here and later hopefully will migrate to proper
 * objects:
 * - The default speaker output object.
 */
class SApplication
    : public QApplication,
      public audio::PlaybackContext,  // app services for twSpeaker (proposal 14, Phase 0)
      public SAppContext              // app services for the core modules (Phase 6)
{
    Q_OBJECT
public:
    SApplication( int &argc, char **argv );
    virtual ~SApplication();
    static SApplication &app();

    std::shared_ptr<twSpeaker> getSpeaker() const;
    tw303aEnvironment *get303aEnvironment() const override;

    SLink *getCurrentSelectedSLink() const;
    bool isSelectionEmpty() const;
    bool isSLinkSelected( SLink * ) const override;
    const SSelectionList &getSelectionList() const;

    // Path-based selection methods (for action-backed operations)
    void setSelectionFromPaths(const QList<QList<int>> &paths) override;
    void addSelectionFromPaths(const QList<QList<int>> &paths) override;
    void removeSelectionFromPaths(const QList<QList<int>> &paths) override;
    void toggleSelectionFromPaths(const QList<QList<int>> &paths) override;
    QList<QList<int>> getCurrentSelectionPaths() const override;

    // SLink-based selection action submission (convenience for UI)
    void submitSetSelectionAction(SLink *link);
    void submitAddSelectionAction(SLink *link);
    void submitToggleSelectionAction(SLink *link);
    void submitClearSelectionAction();

    SProject *getCurrentProject() const override;
    void setCurrentProject( SProject * );
    // Re-fetch the project root component's first output and connect it to
    // the speaker. Call this when the synth graph has changed (tracks
    // added, busses inserted, etc.) so that playback uses the current
    // wiring rather than the snapshot taken at project-creation time.
    void rewireSpeaker() override;
    offset_t getGlobalLocatorPos() const override;   // SAppContext (proposal 37 P5)
    // The last position the user explicitly NAVIGATED the playhead to — a
    // click on the timeline, "0"/Home (go to range start), End (go to range
    // end), or the testkit's `set-locator` verb. Deliberately NOT the same
    // thing as getGlobalLocatorPos(): that one also moves under playback and
    // under Stop's own "second press returns to 0" convenience, neither of
    // which is a navigation. Space resumes playback from HERE (item o);
    // Shift+Space resumes from getGlobalLocatorPos() instead, unchanged.
    offset_t lastNavigatedLocatorPos() const { return lastNavigatedLocatorPos_; }
    // Record a direct user navigation of the playhead (see
    // lastNavigatedLocatorPos above). Called ALONGSIDE setGlobalLocatorPos, at
    // the few call sites that are genuinely a user picking a position — never
    // from playback advance, never from Stop's reset-to-0, never from the
    // count-in/pre-roll or MIDI-record preamble's own internal seeks.
    void noteUserNavigatedLocator( offset_t pos ) { lastNavigatedLocatorPos_ = pos; }
    // Store the playback position from the REALTIME AUDIO THREAD. This only does
    // an atomic store — it must NOT emit any Qt signal or otherwise touch QObject
    // machinery, because doing so from the raw render std::thread makes Qt adopt
    // that thread; the adopted thread's Qt-TLS cleanup then runs during DLL
    // THREAD_DETACH at thread exit and deadlocks the join() in stopOutput(). The
    // UI playhead is instead driven by a main-thread QTimer (see pumpLocator()).
    void setGlobalLocatorPosRealtime( offset_t );
    bool isPlaying() const;
    bool isRenderingActive() const override;
    bool isRecordingActive() const;
    // Proposal 34: is the metering pump running? (Also true during the decay
    // tail after the transport stops.)
    bool isMeteringActive() const;
    // The MASTER level at `pos` — read from the frozen pages of the very
    // component the engine plays (rootComponent()), by the same page-probe
    // mechanism the per-track meters use, so master and tracks agree. False when
    // there is nothing to measure (no project, or no frozen page at pos).
    // Consequence of reading the graph rather than the device: an underrun reads
    // as normal level, not as a dip.
    bool masterLevel( offset_t pos, twLevelSample &out );
    // N-lane form (proposal 36 B8) plus the width to ask for: the mixer root's
    // declared channel count, i.e. the project's.
    bool masterLevel( offset_t pos, twLevelSampleSet &out, int wantLanes );
    int  masterChannels();
    // Locator position captured when the current recording began. The view uses
    // it (with the live locator) to draw the growing in-progress capture region.
    offset_t recordingStartFrame() const { return recordingStartFrame_; }
    void setRecordingStartFrame( offset_t f ) { recordingStartFrame_ = f; }
    SActionHistory *actionHistory() const;
    void submitAction(SAction *action);

    audio::RenderSession *renderSession() const;
    void startRender(const audio::RenderParams &params) override;

    // THE RUN BARRIER (proposal 37 D4 / 4.4). A "run" is one contiguous
    // traversal of the graph by a consumer: an offline render, or a playback
    // start. Instruments are the only CLASS-1 components in the graph whose
    // state is not position-addressed - a synth voice carries its envelope
    // across pages - so a run that inherits the previous run's continuity
    // renders different audio for the same position. Two renders of the same
    // project would then not be byte-identical, which is F4.
    //
    // The barrier is: for every track whose slot 0 is an INSTRUMENT,
    //   1. slot->forgetContinuity()            - the processor stops believing
    //      the next page continues the last one, so it repositions (reset +
    //      chase + pre-roll K, D4) instead of continuing stale voices; and
    //   2. invalidateRenderPathRange(pos, INT64_MAX) - the app-side path walk
    //      up to the root, which is the ONLY thing that carries a change from a
    //      tap up to the components the consumers ask (F13).
    // In that ORDER: a page rendered after the epoch bump is then guaranteed to
    // have seen the cleared continuity, and one rendered in between is staled by
    // the bump and re-rendered. Effects are deliberately NOT barriered - their
    // splice at a page boundary is what they do today.
    //
    // MAIN THREAD ONLY, and always BEFORE the run's first demand: from
    // startRender() before the session thread spawns, and from every play-start
    // path immediately before twSpeaker::startOutput() (which performs the
    // engine's pre-readahead seekTo + startReadahead on this thread). NOT from
    // setGlobalLocatorPos - a stopped locate demands nothing, and requestSeek
    // only runs while playing. NEVER from the readahead thread, and never on a
    // seek during playback or a loop wrap: those keep today's page-boundary
    // splices (a mid-page re-stale would be an audible switch at an arbitrary
    // offset, F14).
    //
    // Idempotent under any ordering: a late barrier costs one re-render, never
    // a wrong page served as current (verify-at-publish self-staleness,
    // schedule inv. 8). A project with no instrument does nothing at all.
    void beginRun( offset_t pos );

    // AUDIO RECORDING (proposal 21 L3b). One recorder per app; it owns the
    // take, the growing clip and the placement. Never null after construction.
    SAudioRecorder *audioRecorder() const { return audioRecorder_.get(); }
    // MIDI RECORDING (proposal 21 L4 = 37 P8b). The SECOND recorder, and the
    // split between them is by TRACK INPUT rather than by two record buttons:
    // an armed track whose `trackInput` is `midi:`/`keyboard` belongs to this
    // one, every other armed track to the audio recorder. A record start runs
    // both, so a project with an armed guitar and an armed synth part records
    // both in one pass. Never null after construction.
    SMidiRecorder *midiRecorder() const { return midiRecorder_.get(); }
    /// Begin a take on every armed track, audio and MIDI. False when nothing
    /// is armed or the audio input would not open for the audio half.
    ///
    /// COUNT-IN AND PRE-ROLL (proposal 21 L5) are handled HERE, around the two
    /// recorders, because they are TRANSPORT behaviours and neither recorder
    /// owns the transport on its own. With either configured and the transport
    /// stopped this returns true immediately and the take begins later, from
    /// `pumpRecordPreamble()`; `recordPreambleActive()` says so.
    bool startRecording();
    /// End the take and commit the placement (one undo step per recorder). A
    /// preamble still running is CANCELLED (nothing was recorded yet).
    void stopRecording();

    // --- count-in / pre-roll (proposal 21 L5) ------------------------------

    /**
     * THE READING TAKEN, stated once, here.
     *
     * COUNT-IN: the click plays for N bars BEFORE the record position while
     * the transport is STOPPED (the playhead sits at the locator), then the
     * transport starts and recording begins AT THE LOCATOR. The placed clip
     * therefore lands exactly where it would have with no count-in, and the
     * capture holds N bars of clicks before the first recorded frame. That is
     * Cubase / Logic / REAPER; the alternative reading -- roll the count-in
     * bars ON the timeline, so the take lands N bars later -- would make the
     * count-in silently move the user's recording.
     *
     * PRE-ROLL: the transport STARTS N bars before the locator and rolls
     * through them (so the arrangement is heard), and recording begins when
     * the playhead reaches the locator. The take is recorded into a run that
     * was already playing, so `SAudioRecorder` sees `wasPlaying_` and the
     * trim floor is the transport start, exactly as for a punch-in.
     *
     * The two compose: the count-in runs first, then the pre-roll rolls.
     */
    bool recordPreambleActive() const { return preamblePhase_ != 0; }
    /// Abandon a preamble in flight. Nothing was recorded, so there is nothing
    /// to commit and nothing to undo.
    void cancelRecordPreamble();
    /// "off" | "countIn N/M" | "preRoll at F" — for the UI and the testkit.
    QString recordPreambleState() const;

    // --- the latency readout (proposal 21 L5, design D5) -------------------

    /// The device's output latency in PROJECT frames, WITHOUT the "only while
    /// playing" gate `meterLatencyFrames()` applies. The readout has to show a
    /// number while the transport is stopped; a compensation must not.
    offset_t outputLatencyFramesProject() const;
    /// The open input device's reported latency in PROJECT frames, or 0.
    offset_t inputLatencyFramesProject() const;
    /**
     * "in 4800 fr (100.0 ms) | out 1024 fr (21.3 ms) | round trip 5824 fr
     * (121.3 ms)" — the transport bar's readout and its tooltip, from the
     * devices that are actually OPEN. Terms whose device is closed read 0.
     */
    QString latencyReport() const;

    // SAppContext: start/stop transport playback (speaker + playing flag).
    void setPlaybackRunning( bool play ) override;

    // audio::PlaybackContext — the speaker's view of the app. rootComponent()
    // and locatorPosition() run on the UI thread; publishPosition() runs on the
    // AUDIO thread (atomic ops only, no Qt).
    //
    // `locatorHeldElsewhere()` is RETIRED (proposal 21 L3b, design D7). It
    // existed because the old recording path drove the playhead from the record
    // worker and had to stop the render callback from also publishing it. A
    // record start now goes through `setPlaybackRunning()` like any other
    // transport start, so THE OUTPUT PUBLICATION IS THE PLAYHEAD AUTHORITY IN
    // EVERY MODE — which is also what the placement conversion's anchor needs
    // (it reads the engine clock, which was being stamped all along while the
    // locator was "held").
    std::shared_ptr<twComponent> rootComponent() override;
    std::uint64_t locatorPosition() override { return getGlobalLocatorPos(); }
    void publishPosition(std::uint64_t absPos) override {
        setGlobalLocatorPosRealtime((offset_t) absPos);
    }

    // How many times the RT thread has published a position in this process.
    // The MIDI-out pump anchors its clock on a PUBLICATION rather than on a
    // position CHANGE: twSpeaker defers the device start until the readahead is
    // primed, so between "play" and the first callback the playhead sits still
    // at the locator, and a due time hung on that static value would put the
    // first bar on the wire before a single frame had been delivered. Written
    // by the audio thread with a relaxed store (single writer, no signal, no
    // Qt), read by the main thread.
    std::uint64_t locatorPublishSeq() const {
        return locatorPublishSeq_.load( std::memory_order_relaxed );
    }

    // Frames to subtract from the published locator so a consumer reads the
    // audio that is being HEARD rather than the audio just handed to the
    // device. 0 when the backend does not report a latency, or nothing is
    // playing. Named for the meters (proposal 34) that first needed it; the
    // MIDI-out pump reuses it verbatim, because the conversion from DEVICE
    // frames at the DEVICE rate to PROJECT frames is the same conversion.
    offset_t meterLatencyFrames() const;
    // The device's buffer size, in PROJECT frames. twSpeaker publishes the
    // position AFTER the pull, so the frame just handed to the device is
    // `published - this`; the MIDI-out pump needs that correction and nothing
    // else does. 0 when unknown.
    offset_t outputBufferFramesProject() const;

    // The MIDI-out pump (proposal 37 P7b). Never null after construction; the
    // Options dialog asks it for the port lists rather than minting a
    // MidiOutput of its own (see SMidiOutPump::outputPorts).
    SMidiOutPump *midiOutPump() const { return midiOutPump_.get(); }
    // The live lane (proposal 21 L1b). Never null after construction; the
    // arm/disarm ordering, the pump and the input device all live in there.
    SLiveMonitor *liveMonitor() const { return liveMonitor_.get(); }
    // The app's MIDI INPUT ports (proposal 21 L2). Never null after
    // construction. Owned here rather than by SLiveMonitor because the MIDI
    // recorder (L4) is a second consumer of the same ports, and neither of
    // them may own the other's device.
    SMidiInputHub *midiInputHub() const { return midiInputHub_.get(); }
    // The Nextcloud accounts model (proposal 38 GATE 5b). Never null after
    // construction; Options -> Media and the media browser's source combo
    // both read it, and it is what installs smedia::CredentialProvider.
    SMediaAccountManager *mediaAccounts() const { return mediaAccounts_.get(); }
    // The computer keyboard as a real MIDI port (design D9). The piano-roll
    // dock reaches it through here: `app/eventui` may not include tw/devices,
    // and routing the two calls through the shell is the same arrangement the
    // arranger's zoom uses.
    void keyboardNoteOn( int key, int velocity, int channel = 0 );
    void keyboardNoteOff( int key, int channel = 0 );
    // SAppContext: a track was armed / disarmed, or its input or monitor mode
    // moved. One plan rebuild, here, on the main thread.
    void liveLanesChanged() override;

    // Test output directory for artifacts (screenshots, renders, etc.)
    void setTestOutputDir(const QString &path);
    QString testOutputDir() const override;
    bool ensureOutputDirExists() const override;

    // --- plugin discovery (proposal 08 M2) ---------------------------------
    // Push the persisted search paths into the engine registry. Called at
    // startup and whenever the options dialog changes the list.
    void pushPluginSearchPaths();
    // Start a background scan (the registry owns the worker thread). force
    // re-probes modules whose previous probe failed or timed out.
    void rescanPlugins( bool force );
    bool isPluginScanActive() const;
    // "N plugins, M modules scanned (K cached, S skipped)" for the options page.
    QString pluginScanStatusText() const;

    // Proposal 37 P6: the Touch/Latch/Write recorder. One per app, because a
    // pass is a transport-wide thing and only one control can be held at a
    // time; it lives here rather than in a view because BOTH the arranger's
    // fader (app/timeline) and the plugin parameter editor (app/pluginui) feed
    // it, and the shell is the only module both may reach.
    SAutomationRecorder &automationRecorder() const { return *automationRecorder_; }

    // App-wide status/mode line shown in the main window's status bar. Views
    // push the active (or hover-telegraphed) gesture here; the main window
    // reflects it. Empty string means "no special mode" (idle).
    const QString &getStatusMode() const { return statusMode_; }

signals:
    void globalLocatorMoved( offset_t newPos, offset_t oldPos );
    // Emitted ONLY from pumpLocator() — i.e. when the playhead advances under
    // playback or recording, never on a manual seek (setGlobalLocatorPos). The
    // view-follows-playhead feature listens to this so manual positioning never
    // scrolls the view.
    void locatorAdvanced( offset_t newPos, offset_t oldPos );
    void statusModeChanged( const QString &mode );
    // Emitted from the MAIN thread once a background plugin scan has finished
    // (see pumpPluginScan). The scan thread must never emit this itself: a Qt
    // signal from a worker thread makes Qt adopt it, and the adopted thread's
    // TLS cleanup deadlocks the teardown join.
    void pluginScanFinished();

    // Proposal 34 — one tick for EVERY meter in the app, ~30 Hz.
    //   pos    is already LATENCY-COMPENSATED (see meterLatencyFrames): it is the
    //          position being HEARD, not the one just handed to the device. Every
    //          meter shares it, so all of them stay mutually consistent.
    //   nowMs  monotonic timestamp for the ballistics, which integrate over the
    //          ACTUAL dt rather than counting ticks.
    //   live   false when the transport is not running. Meters must then decay
    //          (twMeterBallistics::idle) instead of re-measuring a static
    //          position, which would show a steady level forever.
    // A signal rather than a registry: the track heads are deleteLater()'d on
    // every refreshTrackTree(), so their connections drop themselves.
    void meterTick( offset_t pos, qint64 nowMs, bool live );
    // Meters should return to the floor and clear their clip latch. Emitted when
    // the transport starts.
    void meterReset();

public slots:
    // Set the status/mode line. Emits statusModeChanged only when it changes.
    void setStatusMode( const QString &mode );
    void setSelectedSLink( SLink * );        
    void addSelectedSLink( SLink * );
    void clearSelection();
    void unselectSLink( SLink * );
    void setGlobalLocatorPos( offset_t );
    void setPlaying( bool );

private slots:
    void unselectSLink();
    // Main-thread poll: pick up the position the audio thread stored and emit
    // globalLocatorMoved so the playhead repaints. Driven by locatorTimer_ while
    // playing.
    void pumpLocator();
    // Main-thread poll of the engine's plugin scanner: when it goes idle, stop
    // the timer and emit pluginScanFinished from HERE, the main thread.
    void pumpPluginScan();
    // Proposal 34: emit meterTick. Deliberately NOT folded into pumpLocator —
    // that one only does work when the position CHANGED and self-stops the
    // instant playback stops, whereas meters need a tick at a static position
    // (to decay) plus a tail after stop, or the bars freeze mid-level.
    void pumpMeters();
    /// The count-in / pre-roll poll (proposal 21 L5).
    void pumpRecordPreamble();
    // A render suspends every live lane for its duration and comes back as a
    // FRESH arm (proposal 21 design D4). The render session signals completion
    // from ITS OWN thread, so this is reached by a queued invocation and runs
    // where every other model edit runs.
    void resumeLiveAfterRender();

private:
    // The live monitor drives metering and reads the master width; both are
    // app state it has no business duplicating.
    friend class SLiveMonitor;
    void initPluginRegistry();
    // THE METRONOME SWITCH IS A PLAN-REBUILD TRIGGER (proposal 21 L5). A real
    // pointer-to-member-function slot, not a lambda: Qt::UniqueConnection is
    // silently a NO-OP for a functor/lambda connection (it logs "unique
    // connections require a pointer to member function of a QObject
    // subclass" and makes NO connection at all, not even the first one), so
    // the lambda this used to be left the metronome property watched by
    // NOTHING - toggling it off mid-playback changed the project property
    // but never reached SLiveMonitor::refresh(), and the click kept sounding
    // until the next unrelated live-plan rebuild (e.g. a transport stop).
    // See setCurrentProject() for the connect() call.
    void onProjectPropertyChangedForLive( const QString &key, const QVariant &value );
    // Proposal 38 gate 3: the media cache's root/cap and smediadrop's
    // placement hook. See the definition for why the hook exists at all.
    void initMediaLayer();

    // Start (or re-arm) the metering pump. No-op during an offline render.
    void startMetering();

    // How many idle ticks to keep pumping after the transport stops, so every
    // meter can decay all the way to the floor before the timer stops. Worst
    // case is the held peak tick from the top of the scale:
    // holdSec + (ceilDb-floorDb)/holdDecayDbPerSec = 1.5 + 66/12 = 7.0 s.
    static constexpr int METER_TAIL_TICKS = 240;   // ~8 s at 33 ms

    static SApplication *singleton_;
    SSelectionList *selectionList_;
    tw303aEnvironment *t3Env_;
    std::shared_ptr<twSpeaker> t3Speaker_;
    std::shared_ptr<twWhiteNoise> t3WhiteNoise_;
    SActionHistory *actionHistory_;
    std::unique_ptr<audio::RenderSession> renderSession_;
    std::unique_ptr<SAudioRecorder> audioRecorder_;
    std::unique_ptr<SMidiRecorder>  midiRecorder_;   // proposal 21 L4
    // True while a MIDI-only take is running, i.e. one that the AUDIO recorder
    // did not start the transport for. It decides who stops the transport at
    // the end, and it is a flag rather than a re-derivation because the armed
    // set is cleared by the commit before the stop finishes.
    bool midiOwnsTransport_ = false;

    SLink *currentSelectedSLink_;

    // Written by the audio thread (atomic store, no signal) and by the UI thread
    // (setGlobalLocatorPos, which also emits). Read by both.
    std::atomic<offset_t> globalLocatorPos_;
    // Incremented by the AUDIO thread on every publishPosition (see
    // locatorPublishSeq). Atomic store only - no signal, no QObject.
    std::atomic<std::uint64_t> locatorPublishSeq_{ 0 };
    offset_t lastShownLocator_ = 0;   // last position the UI emitted (main thread only)
    offset_t recordingStartFrame_ = 0; // locator at record start (for the live region)
    // See lastNavigatedLocatorPos()/noteUserNavigatedLocator() above (item o).
    // Main-thread only — every call site that updates it already runs there.
    offset_t lastNavigatedLocatorPos_ = 0;

    QTimer *locatorTimer_ = nullptr;  // drives the playhead repaint while playing
    QTimer *pluginScanTimer_ = nullptr;  // polls the background plugin scan
    QTimer *meterTimer_ = nullptr;    // drives meterTick (proposal 34)
    // The count-in / pre-roll sequencer (proposal 21 L5). A 5 ms poll rather
    // than a single-shot QTimer of the count-in's DURATION: the count-in ends
    // when the RT has actually been handed N bars of click, which is a frame
    // count on the live ring and not a wall-clock interval (twlivering.h).
    bool startRecordingNow_();
    void beginPreRoll_( offset_t preRoll );
    void finishRecordPreamble_();
    QElapsedTimer preambleClock_;
    QTimer  *preambleTimer_ = nullptr;
    int      preamblePhase_ = 0;      // 0 off, 1 count-in, 2 pre-roll
    offset_t preambleTarget_ = 0;     // the LOCATOR the take must begin at
    qint64   preambleDeadlineMs_ = 0; // a device that never runs must not hang
    QElapsedTimer meterClock_;        // monotonic ms handed to the ballistics
    int meterTailTicks_ = 0;          // remaining decay ticks after a stop
    twLevelProbe masterProbe_;        // reads the mixer root's frozen pages
    std::unique_ptr<SMidiOutPump> midiOutPump_;   // proposal 37 P7b
    std::unique_ptr<SLiveMonitor> liveMonitor_;   // proposal 21 L1b
    std::unique_ptr<SMidiInputHub> midiInputHub_; // proposal 21 L2
    std::unique_ptr<SMediaAccountManager> mediaAccounts_; // proposal 38 GATE 5b
    std::unique_ptr<SAutomationRecorder> automationRecorder_;  // proposal 37 P6
    bool isPlaying_;
    SProject *currentProject_;
    QString statusMode_;
    QString testOutputDir_;        // directory for test artifacts (screenshots, renders)
};

#endif
