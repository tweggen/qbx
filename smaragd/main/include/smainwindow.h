
#ifndef _SMAINWINDOW_H_
#define _SMAINWINDOW_H_

#include <qmainwindow.h>
#include <qmenubar.h>
#include <QString>
// #include <qpopupmenu.h>

class SProject;
class QAction;
class QActionGroup;

class SMainWindow
    : public QMainWindow
{
    Q_OBJECT
public:
    SMainWindow();
    virtual ~SMainWindow();

protected slots:
    void nyi();
    void fileExit();
    void fileNew();
    void fileSave();
    void fileSaveAs();
    void fileOpen();
    void fileClose();

    void startPlaying();
    void stopPlaying();

    void audioDeviceSelected( QAction * );
    void runTestSequence();
    void runVolumeBurst();
    void runSaveLoadTest();
    void undo();
    void redo();

private:
    void newProject();
    void closeProject();
    void buildAudioMenu();

    void createDocksToolbars();
    void destroyDocksToolbars();

    // Serialize the current project to path; reports errors via a dialog.
    bool saveToPath( const QString &path );
    // Reflect the current project file (or "untitled") in the window title.
    void updateWindowTitle();

    SProject *currentProject_;
    QWidget *projectRootWidget_;
    QString currentFilePath_;   // empty = never saved/loaded (untitled)

    QMenu *qFileMenu_;
    QMenu *qAudioMenu_;
    QMenu *qTestMenu_;
    QActionGroup *deviceGroup_;

    QAction *actStop_, *actPlay_;
    QToolBar *qTBTransport_;
    QDockWidget *qDockExternFileList_;
}; 

#endif
