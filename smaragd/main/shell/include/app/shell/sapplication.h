
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
    offset_t getGlobalLocatorPos() const;
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

    // Frames to subtract from the published locator so a meter reads the audio
    // that is being HEARD rather than the audio just handed to the device.
    // 0 when the backend does not report a latency, or nothing is playing.
    offset_t meterLatencyFrames() const;
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
    offset_t lastShownLocator_ = 0;   // last position the UI emitted (main thread only)
    offset_t recordingStartFrame_ = 0; // locator at record start (for the live region)
    QTimer *locatorTimer_ = nullptr;  // drives the playhead repaint while playing
    QTimer *pluginScanTimer_ = nullptr;  // polls the background plugin scan
    QTimer *meterTimer_ = nullptr;    // drives meterTick (proposal 34)
    QElapsedTimer meterClock_;        // monotonic ms handed to the ballistics
    int meterTailTicks_ = 0;          // remaining decay ticks after a stop
    twLevelProbe masterProbe_;        // reads the mixer root's frozen pages
    bool isPlaying_;
    SProject *currentProject_;
    QString statusMode_;
    QString testOutputDir_;        // directory for test artifacts (screenshots, renders)
};

#endif
