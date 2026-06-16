
#ifndef _SMAINWINDOW_H_
#define _SMAINWINDOW_H_

#include <qmainwindow.h>
#include <qmenubar.h>
#include <QString>
#include <QVariant>
// #include <qpopupmenu.h>

class SProject;
class QAction;
class QActionGroup;
class QLabel;
class SRecordingProgressDialog;

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

protected:
    void closeEvent( QCloseEvent *event ) override;

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
    void onRecordingCompleted();

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
    void runSetClipStretch();
    void runSetClipPitch();
    void undo();
    void redo();
    void showOptionsDialog();

    // Toolbar palette toggles (each submits the matching toggle action).
    void toggleSnapToGrid();
    void toggleGrid();
    void toggleMetronome();
    void toggleCycle();
    // Track grouping toolbar -> act on the arranger's last-clicked track.
    void groupTrack();
    void ungroupTrack();

    // Reflect a project property change on the matching palette button.
    void onProjectPropertyChanged( const QString &key, const QVariant &value );

    // Reflect the app-wide status/mode (slip, time-stretch, …) in the status bar.
    void onStatusModeChanged( const QString &mode );

private:
    // Build the status bar and its permanent widgets (mode indicator, …).
    void buildStatusBar();
    void newProject();
    void closeProject();
    void buildAudioMenu();
    void buildPaletteToolbar();

    void createDocksToolbars();
    void destroyDocksToolbars();
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
    QToolBar *qTBTransport_;
    QToolBar *qTBPalette_;
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
    QAction *actSnapToGrid_, *actGrid_, *actMetronome_, *actCycle_;
    QDockWidget *qDockExternFileList_;

    // Permanent mode indicator on the right of the status bar.
    QLabel *modeLabel_;
};

#endif
