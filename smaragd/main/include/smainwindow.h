
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

    void audioDeviceSelected( QAction * );
    void runTestSequence();
    void runVolumeBurst();
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

    SProject *currentProject_;
    QWidget *projectRootWidget_;
    QString currentFilePath_;   // empty = never saved/loaded (untitled)

    QMenu *qFileMenu_;
    QMenu *qRecentMenu_;        // File -> Open Recent submenu
    QMenu *qAudioMenu_;
    QMenu *qTestMenu_;
    QActionGroup *deviceGroup_;

    QAction *actStop_, *actPlay_;
    QToolBar *qTBTransport_;
    QToolBar *qTBPalette_;
    QToolBar *qTBTracks_;
    QAction *actSnapToGrid_, *actGrid_, *actMetronome_, *actCycle_;
    QDockWidget *qDockExternFileList_;

    // Permanent mode indicator on the right of the status bar.
    QLabel *modeLabel_;
};

#endif
