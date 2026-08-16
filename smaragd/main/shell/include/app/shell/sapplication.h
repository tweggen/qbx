
#ifndef _SAPPLICATION_H
#define _SAPPLICATION_H

#include <memory>
#include <atomic>

#include "tw/graph/tw303aenv.h"
#include "tw/graph/twcomponent.h"
#include "tw/render/render_session.h"
#include "tw/record/recording_session.h"
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
    // Locator position captured when the current recording began. The view uses
    // it (with the live locator) to draw the growing in-progress capture region.
    offset_t recordingStartFrame() const { return recordingStartFrame_; }
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

    audio::RecordingSession *recordingSession() const;
    void startRecording(const audio::RecordingParams &params);

    // SAppContext: start/stop transport playback (speaker + playing flag).
    void setPlaybackRunning( bool play ) override;

    // audio::PlaybackContext — the speaker's view of the app. rootComponent()
    // and locatorPosition() run on the UI thread; locatorHeldElsewhere() and
    // publishPosition() run on the AUDIO thread (atomic ops only, no Qt).
    std::shared_ptr<twComponent> rootComponent() override;
    std::uint64_t locatorPosition() override { return getGlobalLocatorPos(); }
    bool locatorHeldElsewhere() override { return isRecordingActive(); }
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

private:
    void initPluginRegistry();

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
    std::unique_ptr<audio::RecordingSession> recordingSession_;

    SLink *currentSelectedSLink_;

    // Written by the audio thread (atomic store, no signal) and by the UI thread
    // (setGlobalLocatorPos, which also emits). Read by both.
    std::atomic<offset_t> globalLocatorPos_;
    // Incremented by the AUDIO thread on every publishPosition (see
    // locatorPublishSeq). Atomic store only - no signal, no QObject.
    std::atomic<std::uint64_t> locatorPublishSeq_{ 0 };
    offset_t lastShownLocator_ = 0;   // last position the UI emitted (main thread only)
    offset_t recordingStartFrame_ = 0; // locator at record start (for the live region)
    QTimer *locatorTimer_ = nullptr;  // drives the playhead repaint while playing
    QTimer *pluginScanTimer_ = nullptr;  // polls the background plugin scan
    QTimer *meterTimer_ = nullptr;    // drives meterTick (proposal 34)
    QElapsedTimer meterClock_;        // monotonic ms handed to the ballistics
    int meterTailTicks_ = 0;          // remaining decay ticks after a stop
    twLevelProbe masterProbe_;        // reads the mixer root's frozen pages
    std::unique_ptr<SMidiOutPump> midiOutPump_;   // proposal 37 P7b
    bool isPlaying_;
    SProject *currentProject_;
    QString statusMode_;
    QString testOutputDir_;        // directory for test artifacts (screenshots, renders)
};

#endif
