
#ifndef _SAPPLICATION_H
#define _SAPPLICATION_H

#include <memory>
#include <atomic>

#include <tw303aenv.h>
#include <twcomponent.h>
#include <render_session.h>
#include <recording_session.h>
#include <audio/playback_context.h>
#include <QApplication>
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
      public audio::PlaybackContext   // app services for twSpeaker (proposal 14, Phase 0)
{
    Q_OBJECT
public:
    SApplication( int &argc, char **argv );
    virtual ~SApplication();
    static SApplication &app();

    twSpeaker *getSpeaker() const;
    tw303aEnvironment *get303aEnvironment() const;

    SLink *getCurrentSelectedSLink() const;
    bool isSelectionEmpty() const;
    bool isSLinkSelected( SLink * ) const;
    const SSelectionList &getSelectionList() const;

    // Path-based selection methods (for action-backed operations)
    void setSelectionFromPaths(const QList<QList<int>> &paths);
    void addSelectionFromPaths(const QList<QList<int>> &paths);
    void removeSelectionFromPaths(const QList<QList<int>> &paths);
    void toggleSelectionFromPaths(const QList<QList<int>> &paths);
    QList<QList<int>> getCurrentSelectionPaths() const;

    // SLink-based selection action submission (convenience for UI)
    void submitSetSelectionAction(SLink *link);
    void submitAddSelectionAction(SLink *link);
    void submitToggleSelectionAction(SLink *link);
    void submitClearSelectionAction();

    SProject *getCurrentProject() const;
    void setCurrentProject( SProject * );
    // Re-fetch the project root component's first output and connect it to
    // the speaker. Call this when the synth graph has changed (tracks
    // added, busses inserted, etc.) so that playback uses the current
    // wiring rather than the snapshot taken at project-creation time.
    void rewireSpeaker();
    offset_t getGlobalLocatorPos() const;
    // Store the playback position from the REALTIME AUDIO THREAD. This only does
    // an atomic store — it must NOT emit any Qt signal or otherwise touch QObject
    // machinery, because doing so from the raw render std::thread makes Qt adopt
    // that thread; the adopted thread's Qt-TLS cleanup then runs during DLL
    // THREAD_DETACH at thread exit and deadlocks the join() in stopOutput(). The
    // UI playhead is instead driven by a main-thread QTimer (see pumpLocator()).
    void setGlobalLocatorPosRealtime( offset_t );
    bool isPlaying() const;
    bool isRenderingActive() const;
    bool isRecordingActive() const;
    // Locator position captured when the current recording began. The view uses
    // it (with the live locator) to draw the growing in-progress capture region.
    offset_t recordingStartFrame() const { return recordingStartFrame_; }
    SActionHistory *actionHistory() const;
    void submitAction(SAction *action);

    audio::RenderSession *renderSession() const;
    void startRender(const audio::RenderParams &params);

    audio::RecordingSession *recordingSession() const;
    void startRecording(const audio::RecordingParams &params);

    // audio::PlaybackContext — the speaker's view of the app. rootComponent()
    // and locatorPosition() run on the UI thread; locatorHeldElsewhere() and
    // publishPosition() run on the AUDIO thread (atomic ops only, no Qt).
    twComponent *rootComponent() override;
    std::uint64_t locatorPosition() override { return getGlobalLocatorPos(); }
    bool locatorHeldElsewhere() override { return isRecordingActive(); }
    void publishPosition(std::uint64_t absPos) override {
        setGlobalLocatorPosRealtime((offset_t) absPos);
    }

    // Test output directory for artifacts (screenshots, renders, etc.)
    void setTestOutputDir(const QString &path);
    QString testOutputDir() const;
    bool ensureOutputDirExists() const;

    // App-wide status/mode line shown in the main window's status bar. Views
    // push the active (or hover-telegraphed) gesture here; the main window
    // reflects it. Empty string means "no special mode" (idle).
    const QString &getStatusMode() const { return statusMode_; }

signals:
    void globalLocatorMoved( offset_t newPos, offset_t oldPos );
    void statusModeChanged( const QString &mode );

public slots:
    // Set the status/mode line. Emits statusModeChanged only when it changes.
    void setStatusMode( const QString &mode );
    void setSelectedSLink( SLink * );        
    void addSelectedSLink( SLink * );
    void clearSelection();
    void unselectSLink( SLink * );
    void setGlobalLocatorPos( offset_t );
    void setSpeakerMaxVal( sample_t );
    void setPlaying( bool );

private slots:
    void unselectSLink();
    // Main-thread poll: pick up the position the audio thread stored and emit
    // globalLocatorMoved so the playhead repaints. Driven by locatorTimer_ while
    // playing.
    void pumpLocator();

private:
    static SApplication *singleton_;
    SSelectionList *selectionList_;
    tw303aEnvironment *t3Env_;
    twSpeaker *t3Speaker_;
    twWhiteNoise *t3WhiteNoise_;
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
    bool isPlaying_;
    SProject *currentProject_;
    QString statusMode_;
    QString testOutputDir_;        // directory for test artifacts (screenshots, renders)
};

#endif
