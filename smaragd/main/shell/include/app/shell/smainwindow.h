
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
#include <vector>
// #include <qpopupmenu.h>

class SProject;
class QAction;
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
class SMediaBrowserPanel;

class SMainWindow
    : public QMainWindow
{
    Q_OBJECT
public:

    // The view shell (proposal 09 §2). Public so the testkit can assert the
    // tab set without reaching through the central widget by cast.
    class SViewTabs *viewTabs() const { return viewTabs_; }

    // The shell, BUILDING it if this is the first thing to ask. The shell is
    // created lazily with the master editor, so a script that has not yet
    // touched the arranger has none -- the same laziness ensureArranger_()
    // exists for, exposed because assert-tab-set asks about the shell itself
    // rather than about a view inside it.
    class SViewTabs *ensureViewShell();
    SMainWindow();
    virtual ~SMainWindow();

    // Startup: open the newest still-existing entry from the recent list, if
    // any. Leaves an empty workspace when there is nothing to restore.
    void openMostRecent();

    // Startup: a non-invasive open/close probe of the CONFIGURED output
    // device (twSpeaker::probeOutputDevice()), run once the window is up.
    // Called ONLY from main.cpp's interactive branch — never under
    // --test-case or --run-actions, so a headless run never reaches the
    // dialog this can trigger (SApplication::notifyAudioOutputUnavailable()
    // is itself also guarded on isTestCaseMode(), belt and suspenders). A
    // failure falls back to "no output" (see twSpeaker::startOutput()'s
    // contract) and reports through the same path a failed Play uses.
    void checkAudioOutputAtStartup();

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
    // TEST ENTRY POINT (fix/track-list-polish m): drive the horizontal zoom
    // (`SMVActualView::setSecondWidth`) and/or pan (`setLeftOffset`, project
    // frames) directly — the same calls the zoom buttons and the wheel make,
    // which is exactly what triggers a save (SMVActualView's
    // secondWidthChanged/leftOffsetChanged -> saveViewStateToProject()).
    // `secondWidth` <= 0 or `scrollX` < 0 leaves that one untouched.
    bool arrangerSetZoomPan( double secondWidth, qlonglong scrollX );
    // "" when aligned, else a description of the first mismatch. A null
    // QString with no arranger at all is reported as an error by the caller.
    QString arrangerLaneAlignment();

    // TEST ENTRY POINT (fix/track-list-polish l): scroll the vertical
    // scrollbar to its OWN maximum (the number a real mouse wheel is capped
    // by — see SStdMixerView::tkVerticalScrollMaximum()'s comment on why
    // tkSetTopRow(rowCount()-1) would not test this) and report
    // "maxScroll=<int>|lastRowBottom=<int>|canvasHeight=<int>|fullyVisible=
    // true|false" — the numeric form of "does the padding let the true last
    // track end up fully visible". "" when there is no arranger or no rows.
    QString arrangerDescribeScrollRange();

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

    // TEST ENTRY POINT: double-click a clip in the arranger through its real
    // mouse handlers (drag-clip-edge's twin, same routing reason). An EVENT
    // (MIDI) clip opens the event editor for it, matching the real
    // mouseDoubleClickEvent; any other clip is a no-op success.
    bool doubleClickClip( int rowIdx, int clipIdx );

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
    // ...and a synthetic Home/End key press straight to that lane's LIVE
    // fader (item j), through the shell for the same reason. `key` is
    // "Home" or "End"; false for an unknown track path or key name.
    bool sendFaderKey( const QString &trackPath, const QString &key );

    // TEST ENTRY POINT: a real click (press+release, no move) into a track's
    // LANE — the clip-drawing area, not the head — at `time`. Runs the same
    // mouse handlers dragClipEdge does; see SStdMixerView::clickLane. Lands on
    // a clip if one covers `time`, empty lane space otherwise.
    bool clickLane( const QString &trackPath, offset_t time,
                    Qt::KeyboardModifiers mods = Qt::NoModifier );

    // TEST ENTRY POINT: the split command ('s' / "Split object"), unchanged
    // from what a real keypress or menu click runs — SStdMixerView::
    // ctSplitSample() is a public slot, so this just calls it. Splits every
    // clip in the CURRENT SELECTION whose extent strictly contains the
    // locator, or (selection empty) the last-clicked clip.
    bool splitSelectionGesture();

    // TEST ENTRY POINT: paint a level meter carrying a known level into a PNG.
    // The ONLY coverage of SLevelMeter::paintEvent — the describe() assertions
    // check the geometry maths, but nothing else proves the widget draws. Returns
    // false if the grab or the save fails.
    // The set form paints one bar PER LANE (proposal 36 B8), so a stereo grab
    // shows two bars at their own heights rather than one folded one.
    bool grabLevelMeter( const QString &path, const twLevelSampleSet &s,
                         bool vertical, int w, int h );

    // TEST ENTRY POINT (proposal 39 M1): collect the envelope a CLIP's own
    // renderer DRAWS over a timeline window, into `out` (resized to `width`).
    // `clipPath` is an index-path to the clip's SLink, the spelling slip-clip
    // and assert-clip-channels use. Returns false when the path names no clip,
    // the clip has no inline renderer, or the renderer produced no envelope
    // (an event clip: SObjectRenderer::collectEnvelope defaults to false).
    //
    // Here for the same reason describeTrackHead and drag-clip-edge are:
    // testkit may not include app/timeline (testkit CONTRACT inv. 5), and the
    // point of the verb is that the SAME call the painter makes is what runs.
    // It needs NO arranger and NO painter - a collect is expressed on a time
    // window, not on a QPainter (see SEnvelopeWindow).
    bool collectClipEnvelope( const QString &clipPath, offset_t start,
                              length_t length, int width,
                              std::vector<preview_t> &out );

    // TEST ENTRY POINT (proposal 39 M3): the FOLDER-SUM twin of the above -
    // the summed envelope of every descendant of the track at `trackPath`,
    // which is the exact call STrackRendererInline::draw() makes to paint the
    // overlay on that lane. Returns false, writing nothing, when the track is
    // not a folder or nothing below it contributed.
    bool collectTrackChildSumEnvelope( const QString &trackPath, offset_t start,
                                       length_t length, int width,
                                       std::vector<preview_t> &out );

    // TEST ENTRY POINT (proposal 39 M3.10): grab the arranger canvas and
    // MEASURE one lane's pixels, so the overlay's colour relations are asserted
    // from an image rather than eyeballed. Returns a describe()-style line, or
    // an empty string when the track/row cannot be resolved. See
    // main/testkit/CONTRACT.md ("assert-lane-overlay").
    //
    // `bandOnly` (proposal 40 M2, default false = unchanged behaviour):
    // restricts the y-scan to the bottom max(2, laneHeight/5) rows of the
    // lane rect -- the same formula STrackRendererInline::drawFeelFlowBand
    // uses for its own band, over the SAME rect this function already reads
    // (laneTop()+1 .. +laneHeight()-2), so the scanned rows are exactly the
    // band's own rows. Needed because a CLIP-covered lane is not a clean
    // background for this verb even outside any overlay: a clip's own
    // waveform paint lands pixels in the "strictly between fill and clip
    // body" luminance range across the WHOLE lane, which the feel-flow
    // heatmap's own subject (a lane that always holds a clip) cannot avoid
    // the way proposal 39's clip-free negative controls did.
    QString describeLaneOverlay( const QString &trackPath, int w, int h,
                                 const QString &pngPath, bool bandOnly = false );

    // TEST ENTRY POINT (proposal 39 M3a): fold a folder lane SHUT, or open it
    // again. ABSOLUTE, never a toggle - a script that says what it wants is
    // idempotent and can be read without counting how many times it ran.
    // It drives SStdMixerView::toggleTrackCollapsed(), the same call the head's
    // fold triangle makes, rather than writing the collapsed set directly: the
    // row rebuild, the head column and the take/automation sub-lane pruning all
    // hang off that one call, and a second spelling of "collapsed" would be
    // free to drift from it. False when the path names no lane.
    bool setTrackCollapsed( const QString &trackPath, bool collapsed );

    // TEST ENTRY POINT (fix/track-list-polish m): read fold state back
    // without needing the arranger to exist (STrack::isCollapsed() directly),
    // so a save/load round-trip case can check it before ever opening a view.
    bool isTrackCollapsed( const QString &trackPath );
    // TEST ENTRY POINT (fix/track-list-polish m): the arranger's zoom/pan as
    // "secondWidth=<double>|scrollX=<qulonglong frames>", the same two values
    // SMVActualView::saveViewStateToProject() persists. "" when there is no
    // arranger.
    QString arrangerDescribeView();

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
    //   scrollEventEditorKeys — drives the piano roll's REAL vertical key
    //                          scrollbar (setValue()), so a case can prove
    //                          the full 0-127 MIDI range is reachable rather
    //                          than only asserting the clamp math. Returns
    //                          the resulting topKey(), or -1 with no piano
    //                          roll bound (kind mismatch or no clip).
    //   virtualKey           — one virtual-keyboard note at the locator, which
    //                          submits `add-note`. False when there is no event
    //                          clip to write into.
    QString describeEventEditor( const QString &clipPath, const QString &kind );
    bool grabEventEditor( const QString &path, int w, int h );
    bool dragNote( const QString &clipPath, qint64 tick, int key, int channel,
                   qint64 toTick, int toKey, const QString &edge,
                   const QString &lane, double toValue );
    int  scrollEventEditorKeys( const QString &clipPath, int topKey );
    bool virtualKey( int key, double velocity, qint64 durationTicks );
    //   virtualKeyHold / Release — PLAY the virtual keyboard rather than write
    //   into a clip (proposal 21 L2): a note-on / note-off pair on the computer
    //   keyboard's in-process MIDI port, which is what a live-armed instrument
    //   track hears. Same rule as every other test verb here: the REAL dock
    //   runs, so the case exercises the widget rather than the script.
    bool virtualKeyHold( int key, double velocity );
    bool virtualKeyRelease( int key );
    /// The keyboard dock's describe(), for assert-event-editor.
    QString describeVirtualKeys() const;

    // TEST ENTRY POINTS for the media browser (proposal 38 gate 2). The dock
    // is built in the ctor and NEVER shown in a headless run, so these push it
    // its state explicitly (trap T10) and describe() is the oracle. They live
    // here for the usual reason (testkit CONTRACT inv. 5: testkit may not
    // include app/timeline) plus one that is specific to the drag:
    // mediaBrowserDrag has to reach BOTH the panel and the arranger, and the
    // shell is the only module that sees both.
    //
    //   mediaBrowserSetSource  — select a source by id and OPEN it.
    //   mediaBrowserSetPath    — set the browse root, optionally EXPANDING one
    //                            directory row through the real itemExpanded.
    //   mediaBrowserActivate   — DOUBLE-CLICK a row by name, through the real
    //                            itemDoubleClicked slot: a directory navigates
    //                            into itself, a file is a no-op.
    //   mediaBrowserSearch     — enter (or, with an empty needle, leave) search
    //                            mode. `viaDebounce` lets the real 250 ms timer
    //                            fire instead of flushing it.
    //   mediaBrowserSetFilter  — the media-type checkbox menu.
    //   mediaBrowserDrag       — build the REAL QMimeData the panel's startDrag
    //                            builds and hand it to the REAL
    //                            SMVActualView::dropEvent. There is no shortcut
    //                            around the drop handler: a verb that submitted
    //                            add-sample itself would pass while the gesture
    //                            was broken. `belowLastTrack` targets the EMPTY
    //                            canvas space below the last lane instead of a
    //                            track (trackPath is then ignored — there is no
    //                            track to resolve yet, that being the point).
    //   describeMediaBrowser   — SMediaBrowserPanel::describe().
    //   mediaBrowserBusy       — is any request the panel displays still live?
    bool mediaBrowserSetSource( const QString &sourceId );
    bool mediaBrowserSetPath( const QString &path, const QString &expandRow );
    bool mediaBrowserActivate( const QString &rowName );
    bool mediaBrowserSearch( const QString &needle, bool recursive,
                             bool viaDebounce );
    bool mediaBrowserSetFilter( const QString &categories );
    bool mediaBrowserDrag( int row, const QString &name,
                           const QString &trackPath, offset_t timePos,
                           bool belowLastTrack = false );
    QString describeMediaBrowser() const;
    bool    mediaBrowserBusy() const;

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

public slots:
    // "Go to project start" / "Go to project end" (item j): jump to the time
    // selection's start/end if one is set, otherwise to frame 0 / the end of
    // the project's arranged content. Bound to "0"/Home and End respectively.
    // Public (and slots, for the old-style SIGNAL/SLOT connect below) because
    // a fader widget's own event filter calls these directly, so Home/End
    // always drive the transport even while the fader has keyboard focus —
    // QAbstractSlider's own keyPressEvent would otherwise treat them as
    // "jump this fader to its minimum/maximum".
    void gotoRangeStart();
    void gotoRangeEnd();

    // Space / Shift+Space (item o). Public for the same reason as above: the
    // `press-play` testkit verb drives these directly to exercise exactly
    // what the two shortcuts do, since `toggle-playback` bypasses this
    // resume-position logic entirely (it drives SAppContext::setPlaybackRunning
    // straight, which is the older, simpler mechanism).
    void startPlaying();              // resume from the LAST NAVIGATED position
    void startPlayingFromCurrent();   // resume from the CURRENT position (unchanged)
    // The real Stop button/shortcut. Public for the same reason, driven by
    // the `press-stop` testkit verb: pressed while ALREADY stopped it resets
    // the locator to 0 — a SEPARATE mechanism from item o's "last navigated"
    // tracking (noteUserNavigatedLocator is deliberately NOT called here), and
    // exercising the real slot is what proves the two do not interfere.
    void stopPlaying();

    // Show + raise the event editor dock, ACTIVELY re-querying the current
    // selection (or binding an explicit clip) rather than trusting only the
    // reactive arrangementChanged -> refresh() connection: a headless test
    // run never routes a project through fileNew()/openProjectFile(), so
    // that connection is never wired there at all, and even in the
    // interactive app a dock merely toggled visible again must show what is
    // ALREADY selected rather than wait for a future selection change
    // (CONTRACT.md inv. 4/9). Public because the arranger's double-click
    // handler calls it. clipPath empty = follow the current selection.
    void showEventEditor( const QString &clipPath = QString() );

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

    void onRecordTriggered();
    void onRecordingFinished();

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
    // Right-click on the transport toolbar's metronome button: "Click while
    // recording" (SOpt::ClickWhileRecording) and "Count in while recording"
    // (a convenience toggle over SOpt::CountInBars - see soptions.h).
    void showMetronomeContextMenu();
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
    // Build the master editor and install it as tab 0 (proposal 09 §2).
    void installMasterEditor_();
    // Build the status bar and its permanent widgets (mode indicator, …).
    void buildStatusBar();
    void newProject();
    void closeProject();
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
    // Shared by startPlaying() (Space) and startPlayingFromCurrent()
    // (Shift+Space) — item o. fromLastNavigated selects which position a
    // fresh play start seeks to; the toggle-to-stop branch is identical
    // either way.
    void startPlayingFrom_( bool fromLastNavigated );

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
    // The view shell (proposal 09). projectRootWidget_ IS this widget now --
    // kept under its old name because the repaint/focus reach-throughs in this
    // file do not care which widget it is, only the casts did.
    class SViewTabs *viewTabs_ = nullptr;
    QWidget *projectRootWidget_;
    QString currentFilePath_;   // empty = never saved/loaded (untitled)

    QMenu *qFileMenu_;
    QMenu *qRecentMenu_;        // File -> Open Recent submenu
    QMenu *qTestMenu_;

    QAction *actStop_, *actPlay_, *actRecord_, *actGotoStart_;
    // Shift+Space (item o) and End (item j) — window-wide shortcuts with no
    // toolbar/menu entry of their own, exactly like actGotoStart_.
    QAction *actPlayFromCurrent_ = nullptr;
    QAction *actGotoEnd_ = nullptr;
    QAction *actSaveAs_ = nullptr;      // File->Save as...; disabled with no project
    QToolBar *qTBTransport_;
    // Proposal 34: master level meter, right of the tempo box. Mono today (the
    // sink duplicates one bus); stereo comes for free when the mixer grows a
    // second one.
    SLevelMeter *qMasterMeter_ = nullptr;
    // THE LATENCY READOUT (proposal 21 L5, design D5). Round trip in ms on the
    // transport bar, the full breakdown in the tooltip. It reads the devices
    // that are OPEN, so it is 0 ms with nothing running and changes the moment
    // a device does - which is the whole point: "why is my monitoring late" is
    // a question the transport bar should be able to answer.
    QLabel *qLatencyLabel_ = nullptr;
    QString lastLatencyText_;
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
    // SApplication::audioOutputDeviceFailed() — shows the "audio device
    // unavailable" dialog with a shortcut into Options → Audio.
    void onAudioOutputDeviceFailed();
    // Shared by showOptionsDialog() (page 0) and onAudioOutputDeviceFailed()
    // (the Audio page, index 1 — see SOptionsDialog's tree/stack order).
    void openOptionsDialogAt( int pageIndex );
    QAction *actSnapToGrid_, *actGrid_, *actMetronome_, *actCycle_;
    // "Count in while recording" (metronome context menu) is a convenience
    // on/off toggle over SOpt::CountInBars, whose own spinbox already treats
    // 0 as "off" (soptions.h). Unchecking stashes the bar count here and
    // writes 0; checking restores it, or falls back to 2 bars - the smallest
    // count-in any qxa case (record_count_in) exercises - if nothing was ever
    // stashed this session. NOT persisted across a restart: that is the
    // "or default" half of the task's own wording, not an oversight.
    int metronomeLastCountInBars_ = 2;
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

    // The SEVENTH dock (proposal 38 gate 2). Created in the ctor beside the
    // other six and hidden on a first run, for the reason every dock here is:
    // shell CONTRACT inv. 4 fixes the restore order (openMostRecent() ->
    // restoreWindowLayout() -> show()), and restoreState() can only place docks
    // that already exist. Its objectName, "dock_media_browser", is the whole
    // persistence mechanism -- docked/floating/closed round-trips through the
    // existing ui/windowState blob and needs no settings key.
    QDockWidget        *qDockMediaBrowser_ = nullptr;
    SMediaBrowserPanel *mediaBrowser_      = nullptr;
    QAction *actClipProps_ = nullptr;   // the F2 (default) binding

    // Permanent mode indicator on the right of the status bar.
    QLabel *modeLabel_;
};

#endif
