
#ifndef _SMAINWINDOW_H_
#define _SMAINWINDOW_H_

#include <qmainwindow.h>
#include <qmenubar.h>
// #include <qpopupmenu.h>

class SProject;
class QAction;
class QActionGroup;

class QString;

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
    void fileOpen();

    void startPlaying();
    void stopPlaying();

    void audioDeviceSelected( QAction * );
    void runTestSequence();

private:
    void newProject();
    void closeProject();
    void buildAudioMenu();

    void createDocksToolbars();
    void destroyDocksToolbars();

    SProject *currentProject_;
    QWidget *projectRootWidget_;

    QMenu *qFileMenu_;
    QMenu *qAudioMenu_;
    QMenu *qTestMenu_;
    QActionGroup *deviceGroup_;

    QAction *actStop_, *actPlay_;
    QToolBar *qTBTransport_;
    QDockWidget *qDockExternFileList_;
}; 

#endif
