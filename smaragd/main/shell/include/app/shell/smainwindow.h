
#ifndef _SMAINWINDOW_H_
#define _SMAINWINDOW_H_

#include <qmainwindow.h>
#include <qmenubar.h>
#include "tw/graph/tw303aenv.h"   // offset_t (used in the signatures below)
#include "tw/metering/tw_level_scan.h"   // twLevelSampleSet (grabLevelMeter)
#include <QString>
#include <QVariant>
#include <QDoubleSpinBox>
#include <QPointer>
// #include <qpopupmenu.h>

class SProject;
class QAction;
class QActionGroup;
class QLabel;
class QToolBar;
class SRecordingProgressDialog;
class SGridToolbar;
class SExternFileList;
class SLogView;
class SLevelMeter;
class STrackDetailPanel;
class SClipPropertiesPanel;
class SEventEditorDock;
class SVirtualKeyboardDock;

class SMainWindow
    : public QMainWindow
{
    Q_OBJECT
public:
    SMainWindow();
    virtual ~SMainWindow();

    // Startup: open the newest still-existing entry from the recent list, if
    // any. Leaves an empty workspace when there is nothing to restore.
    void openMostRecent();

    // TEST ENTRY POINT: forward a clip-edge drag to the arranger, which runs it
    // through its real mouse handlers. Lives here because the testkit module may
    // not include app/timeline (see tools/check_layering.py); shell may.
    // See SStdMixerView::dragClipEdge for the semantics and its limits.
    // grabWhere: 0 = start edge, 1 = end edge, 2 = body (SStdMixerView::ClipGrab).
    bool dragClipEdge( int rowIdx, int clipIdx, int grabWhere, offset_t dropTime,
                       bool upperHalf, Qt::KeyboardModifiers mods = Qt::NoModifier );

    // TEST ENTRY POINT: arranger lane geometry — move the view (zoom / scroll /
    // per-track lane height / take lanes), then check that the track heads
    // still sit exactly on their lanes. Same routing reason as dragClipEdge:
    // testkit may not include app/timeline, shell may.
    // Any argument may be left at its "no change" value (-1 / 0).
    bool arrangerSetLaneView( int laneScaleRow, double laneScale,
                              int toggleTakesRow, int baseTrackHeight,
                              int topRow );
    // "" when aligned, else a description of the first mismatch. A null
    // QString with no arranger at all is reported as an error by the caller.
    QString arrangerLaneAlignment();

    // TEST ENTRY POINT: build the REAL track head at `headHeight` and return its
    // meter's SLevelMeter::describe() string. Goes through the shell because the
    // testkit may not include app/timeline (testkit CONTRACT inv. 5 — the same
    // reason drag-clip-edge lands here), and it is the only automated coverage
    // the meter's density rules can have. Empty string on failure.
    // trackPath is an index-path from the root mixer ("2", "0,1"), so a track
    // NESTED inside a folder can be described. A one-element path is exactly the
    // old top-level index, which is what keeps existing callers spelling-compatible.
    // headWidth <= 0 uses SMV_TRACK_CTRL_WIDTH, the minimal column. Proposal 36
    // B8 needs BOTH widths: the head reshapes on width as well as height
    // (wideMode_ lays the fader and the meter down), so a density claim made at
    // one column width is half a claim.
    QString describeTrackMeter( const QString &trackPath, int headHeight,
                                int headWidth = 0 );
    // TEST ENTRY POINT: paint that same head into a PNG (AC B8.4's evidence).
    bool grabTrackHead( const QString &trackPath, const QString &path,
                        int headHeight, int headWidth,
                        const twLevelSampleSet &level );
    // Testkit: drive the arranger's Group/Ungroup gestures on a lane addressed
    // by index-path, so a NESTED lane can be exercised.
    bool groupTrackGesture( const QString &trackPath, bool ungroup );

    // Testkit: the multi-track selection. selectTrackGesture() is one REAL
    // head click with modifiers (plain / ctrl / shift), so the click semantics
    // themselves are what runs; toggleTrackHead() presses a head's M/S/R
    // button, so a broadcast over the selection is what runs. Both address the
    // lane by index-path and go through the shell because testkit may not
    // include app/timeline (testkit CONTRACT inv. 5).
    bool selectTrackGesture( const QString &trackPath, Qt::KeyboardModifiers mods );
    bool toggleTrackHead( const QString &trackPath, const QString &which, bool on );
    // ...and a grip-drag of a head onto a lane row (nest) or onto its top
    // boundary (reorder / pop out), which is the only route to the multi-track
    // move arithmetic in endTrackDrag.
    bool dragTrackHead( const QString &trackPath, int targetRow, bool nestOnto );

    // TEST ENTRY POINT: paint a level meter carrying a known level into a PNG.
    // The ONLY coverage of SLevelMeter::paintEvent — the describe() assertions
    // check the geometry maths, but nothing else proves the widget draws. Returns
    // false if the grab or the save fails.
    // The set form paints one bar PER LANE (proposal 36 B8), so a stereo grab
    // shows two bars at their own heights rather than one folded one.
    bool grabLevelMeter( const QString &path, const twLevelSampleSet &s,
                         bool vertical, int w, int h );

    // TEST ENTRY POINT (proposal 37 P4): build the REAL track head at
    // `headHeight` and return SSMVMixerControl::describeHead() — the density
    // rules for the instrument "I" and automation "A" buttons, and whether the
    // strip still FITS the lane it was given. Sibling of describeTrackMeter,
    // and here for the same reason: testkit may not include app/timeline
    // (testkit CONTRACT inv. 5). Empty string when the path names no lane.
    QString describeTrackHead( const QString &trackPath, int headHeight );
    // ...and paint that same off-screen head into a PNG. Coverage, not oracle
    // (proposal 37 P6 AC3): the describe() assertions check the maths, nothing
    // else proves the strip - and now the automation button's mode colour -
    // actually draws.
    bool grabTrackHead( const QString &path, const QString &trackPath,
                        int headHeight, int w, int h );

    // TEST ENTRY POINTS for automation (proposal 37 P6). All four go through
    // the shell for the usual reason: testkit may not include app/timeline
    // (tools/check_layering.py, testkit CONTRACT inv. 5).
    //
    //   dragAutomationPoint  — one REAL press/move/release on an automation
    //                          sub-lane (or, while envelopes are armed, on a
    //                          clip's `cut:Gain` overlay). The drag-clip-edge
    //                          twin.
    //   showAutomationLane   — show/hide one lane on a track, i.e. what the
    //                          "Show automation >" picker does.
    //   setClipEnvelopeEdit  — arm clip-envelope editing (OFF by default, so
    //                          every clip-body gesture is untouched until a
    //                          case says otherwise).
    //   grabArrangerLanes    — paint the arranger CANVAS into a PNG.
    bool dragAutomationPoint( const QString &owner, const QString &target,
                              int slotIndex, int take, offset_t time,
                              double value, offset_t toTime, double toValue,
                              Qt::KeyboardModifiers mods );
    bool showAutomationLane( const QString &trackPath, const QString &target,
                             int slotIndex, bool show );
    bool setClipEnvelopeEdit( bool on );
    bool grabArrangerLanes( const QString &path, int w, int h );

    // TEST ENTRY POINTS for the event editor (proposal 37 P4). The dock is
    // built in the ctor and never shown in a headless run, so these drive the
    // REAL widget rather than a re-spelling of it.
    //
    //   describeEventEditor  — bind the dock to `clipPath` (empty = follow the
    //                          selection) and return SEventEditorDock::
    //                          describe(); `kind` switches the editor kind
    //                          first when non-empty.
    //   grabEventEditor      — paint the dock into a PNG. Coverage, not oracle.
    //   dragNote             — one REAL press/move/release on the piano roll.
    //   virtualKey           — one virtual-keyboard note at the locator, which
    //                          submits `add-note`. False when there is no event
    //                          clip to write into.
    QString describeEventEditor( const QString &clipPath, const QString &kind );
    bool grabEventEditor( const QString &path, int w, int h );
    bool dragNote( const QString &clipPath, qint64 tick, int key, int channel,
                   qint64 toTick, int toKey, const QString &edge,
                   const QString &lane, double toValue );
    bool virtualKey( int key, double velocity, qint64 durationTicks );

    // Log dock control, for the log-stress test action (testkit may not include
    // app/servicesui, so it reaches the dock through the shell — the same route
    // drag-clip-edge uses to reach the arranger).
    void setLogDockVisible( bool visible );
    int  logViewBacklog() const;
    int  logViewRows() const;
    qint64 logViewWorstDrainMs() const;
    void   logViewResetDrainStats();

    // Startup: restore the saved window geometry and toolbar/dock layout.
    // Must be called only after the full UI exists (central widget included) —
    // QMainWindow::restoreState() applied to a window without its central
    // widget freezes the layout at the pre-show size once the geometry restore
    // recreates the window directly maximized (no resize transition ever
    // arrives to re-fit it). Returns true if a saved geometry was applied.
    bool restoreWindowLayout();

    // Post a transient hint to the status bar (auto-dismisses after durationMs).
    void postHint( const QString &text, int durationMs = 5000 );

    // Show + raise + focus the clip properties dock (proposal 31). Bound to F2
    // by default and reached from the clip context menu. Public because the
    // arranger's context menu calls it. Toggles closed when it already has
    // focus, so the binding round-trips.
    void showClipProperties();

protected:
    void closeEvent( QCloseEvent *event ) override;
    // Watches the tempo box so Return commits the value and then hands the
    // keyboard back to whatever had it (see the ctor's focusChanged hook).
    bool eventFilter( QObject *watched, QEvent *event ) override;

protected slots:
    void nyi();
    void fileExit();
    void fileNew();
    void fileSave();
    void fileSaveAs();
    void fileOpen();
    void fileClose();
    void onRenderTriggered();

    void startPlaying();
    void stopPlaying();
    void gotoRangeStart();
    void onRecordTriggered();
    void onRecordingFinished();

    void audioDeviceSelected( QAction * );
    void runTestSequence();
    void runVolumeBurst();
    void runTestRender();
    void runSetTimeSelection();
    void runSaveLoadTest();
    void runGroupTrackTest();
    void runReorderTrackTest();
    void runGroupPersist();
    void runUndoRemoveTest();
    void undo();
    void redo();
    void showOptionsDialog();
    // Opened from the resources dock's context menu (SExternFileList).
    void showCleanupDialog();

    // Toolbar palette toggles (each submits the matching toggle action).
    void toggleSnapToGrid();
    void toggleGrid();
    void toggleMetronome();
    void toggleCycle();
    // Track grouping toolbar -> act on the arranger's last-clicked track.
    void groupTrack();
    void ungroupTrack();
    // Transport toolbar tempo box -> push the value to the current project.
    void onTempoSpinChanged( double bpm );

    // Reflect a project property change on the matching palette button.
    void onProjectPropertyChanged( const QString &key, const QVariant &value );

    // Reflect the app-wide status/mode (slip, time-stretch, …) in the status bar.
    void onStatusModeChanged( const QString &mode );

private:
    // The arranger for the current project, creating it if the headless test
    // path has not gone through openProject(). NULL when there is no project.
    class SStdMixerView *ensureArranger_();
    // Build the status bar and its permanent widgets (mode indicator, …).
    void buildStatusBar();
    void newProject();
    void closeProject();
    void buildAudioMenu();
    void buildPaletteToolbar();

    void createDocksToolbars();
    void destroyDocksToolbars();
    // Point the track-detail dock at the current project's mixer selection (or
    // clear it when there is no project). The dock is persistent and outlives
    // every project, so the mixer connection is (re)made on each open/new and
    // dropped again before the project dies.
    void attachTrackDetail();
    void detachTrackDetail();
    // Same lifecycle for the clip properties dock (proposal 31): it follows the
    // SELECTION, and every selection change is an action, so it refreshes off
    // the project's arrangementChanged rather than a signal of its own.
    void attachClipProperties();
    void detachClipProperties();
    // Same lifecycle again for the event editor dock (proposal 37 P4): a
    // selection follower, refreshed off arrangementChanged.
    void attachEventEditor();
    void detachEventEditor();
    // Keep the editor's time axis on the arranger's zoom/scroll while the
    // "Link" toggle is on. Wired HERE because the shell is the only module
    // that sees both app/timeline and app/eventui — the editor deliberately
    // does not depend on the arranger.
    void linkEventEditorAxis();
    // Enable + sync the palette buttons to a project's properties (or disable
    // them when project == NULL), and connect to its propertyChanged signal.
    void syncPaletteToProject( SProject *project );
    // Push the current cycle on/off state and time-range bounds to the speaker
    // so loop playback follows the project. Called when playback starts and
    // whenever the Cycle flag or the range markers change.
    void syncCyclePlayback();

    // Measure and cache audio device latencies on startup if not already known.
    // Shows a modal "Checking audio devices..." dialog to the user.
    void measureAudioLatenciesIfNeeded();

    // Serialize the current project to path; reports errors via a dialog.
    bool saveToPath( const QString &path );
    // Load a project file (shared by File→Open, the recent list, and startup).
    // Closes the current project, loads `fileName`, updates the recent list and
    // window title. Returns false (with a dialog) on a load failure.
    bool openProjectFile( const QString &fileName );
    // Rebuild the File→Open Recent submenu from SSettings::recentProjects().
    void updateRecentMenu();
    // Reflect the current project file (or "untitled") in the window title.
    void updateWindowTitle();

    // Check if the current project has unsaved changes.
    bool hasUnsavedChanges() const;
    // Prompt user to save unsaved changes. Returns true if the user didn't cancel.
    // (returns false only if user clicked Cancel; calls closeProject() if not saving)
    bool promptSaveUnsavedChanges();

    SProject *currentProject_;
    QWidget *projectRootWidget_;
    QString currentFilePath_;   // empty = never saved/loaded (untitled)

    QMenu *qFileMenu_;
    QMenu *qRecentMenu_;        // File -> Open Recent submenu
    QMenu *qAudioMenu_;
    QMenu *qTestMenu_;
    QActionGroup *deviceGroup_;

    QAction *actStop_, *actPlay_, *actRecord_, *actGotoStart_;
    QAction *actSaveAs_ = nullptr;      // File->Save as...; disabled with no project
    QToolBar *qTBTransport_;
    // Proposal 34: master level meter, right of the tempo box. Mono today (the
    // sink duplicates one bus); stereo comes for free when the mixer grows a
    // second one.
    SLevelMeter *qMasterMeter_ = nullptr;
    QDoubleSpinBox *tempoSpin_ = nullptr;  // Transport tempo (BPM) box
    // Who had the keyboard before the tempo box took it, so Return can give it
    // back. Cleared implicitly — always re-checked for liveness before use.
    QPointer<QWidget> tempoPrevFocus_;
    SGridToolbar *qTBPalette_;
    QToolBar *qTBTracks_;
    SRecordingProgressDialog *recordingProgressDialog_ = nullptr;
    // Locator position when recording began. The playhead advances during the
    // capture, so the live locator can't be used to place the recorded cut;
    // we remember the start here and place the cut there.
    offset_t recordingStartPos_ = 0;
    // Latency sync offset (in frames) between input and output devices.
    // Applied to the recorded clip placement to align with simultaneous playback.
    // Positive value means clip should be placed earlier (input is faster than output).
    int64_t recordingLatencySyncOffset_ = 0;
    // The input/output endpoint-rate mismatch warning is shown at most ONCE per
    // session: a modal before every take would be intolerable, and the user may
    // legitimately decide to record anyway.
    bool endpointRateWarningShown_ = false;
    void warnOnEndpointRateMismatch();
    QAction *actSnapToGrid_, *actGrid_, *actMetronome_, *actCycle_;
    QDockWidget *qDockExternFileList_;
    SExternFileList *externFileList_;

    // Track detail dock, docked below the extern file list in the left area.
    // Like the file list it is persistent and project-independent; its content
    // follows the current mixer's selected track via trackDetailConn_.
    QDockWidget       *qDockTrackDetail_  = nullptr;
    STrackDetailPanel *trackDetailPanel_  = nullptr;
    QMetaObject::Connection trackDetailConn_;

    // The log dock (proposal 24). Its objectName is what lets the existing
    // saveState/restoreState persistence restore its visibility and placement,
    // so it needs no settings key of its own.
    QDockWidget *qDockLog_ = nullptr;
    SLogView    *logView_  = nullptr;

    // Clip properties dock (proposal 31). One window for the whole app; the
    // objectName is what carries its docked/floating placement across sessions
    // through the existing saveState/restoreState round trip, so it needs no
    // settings key of its own either.
    QDockWidget          *qDockClipProps_ = nullptr;
    SClipPropertiesPanel *clipPropsPanel_ = nullptr;
    QMetaObject::Connection clipPropsConn_;
    QDockWidget          *qDockEventEditor_ = nullptr;
    SEventEditorDock     *eventEditor_      = nullptr;
    QMetaObject::Connection eventEditorConn_;
    // The two axis links, kept so a re-link (a new project, a testkit call)
    // replaces them instead of stacking a second lambda on the same signal.
    QMetaObject::Connection axisZoomConn_;
    QMetaObject::Connection axisScrollConn_;
    QDockWidget          *qDockVirtualKeys_ = nullptr;
    SVirtualKeyboardDock *virtualKeys_      = nullptr;
    QAction *actClipProps_ = nullptr;   // the F2 (default) binding

    // Permanent mode indicator on the right of the status bar.
    QLabel *modeLabel_;
};

#endif
