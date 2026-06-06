
#include <stdlib.h>

#include <cassert>
#include <QDebug>
#include <qmessagebox.h>
#include <qaction.h>
#include <QActionGroup>
#include <QMenu>
#include <qtoolbar.h>
#include <QDockWidget>
#include <qfile.h>
#include <qfiledialog.h>
#include <QFileInfo>
#include <QStatusBar>

#include <iostream>

#include "sapplication.h"
#include "smainwindow.h"
#include "sobject.h"
#include "sproject.h"
#include "sexternfilelist.h"
#include "slink.h"
#include "sprojectloader.h"
#include "ssettings.h"
#include "sactionhistory.h"
#include <QUndoStack>

#include "sstdmixer.h"

#include "twspeaker.h"

#include "actions/saddtrackaction.h"
#include "actions/saddsampleaction.h"
#include "actions/ssettrackvolumeaction.h"
#include "actions/ssaveprojectaction.h"
#include "actions/sloadprojectaction.h"
#include "actions/stoggleplaybackaction.h"

#include "pix/playoff.xpm"
#include "pix/playon.xpm"
#include "pix/stopoff.xpm"

using namespace std;

void SMainWindow::nyi()
{
    QMessageBox::information( nullptr, "Smaragd warning",
                              "This feature is not yet implemented.",
                              QMessageBox::Ok );
}

void SMainWindow::destroyDocksToolbars()
{
    // No more extern files here.
    delete qDockExternFileList_;
    qDockExternFileList_ = NULL;
}

void SMainWindow::createDocksToolbars()
{
    // Create the extern file list.
    qDockExternFileList_ = new QDockWidget( "Extern file list" );
    SExternFileList *efl = new SExternFileList( qDockExternFileList_, *currentProject_ );
    qDockExternFileList_->setWidget(efl);
    addDockWidget( Qt::LeftDockWidgetArea, qDockExternFileList_ );
}

bool SMainWindow::saveToPath( const QString &path )
{
    if( !currentProject_ ) return false;

    // Same action the round-trip test and any future script would use.
    SSaveProjectAction action( path );
    SApplyResult r = action.apply( currentProject_ );
    if( !r.applied ) {
        QMessageBox::warning( this, "Smaragd",
                              QString( "Could not write project file:\n%1" )
                                  .arg( path ),
                              QMessageBox::Ok );
        return false;
    }

    currentFilePath_ = path;
    SSettings::instance().setLastDir( "project",
                                      QFileInfo( path ).absolutePath() );
    updateWindowTitle();
    statusBar()->showMessage( QString( "Saved %1" )
                                  .arg( QFileInfo( path ).fileName() ), 2000 );
    return true;
}

void SMainWindow::fileSave()
{
    if( !currentProject_ ) return;

    // No path yet (untitled project) -> behave like Save As.
    if( currentFilePath_.isEmpty() ) {
        fileSaveAs();
        return;
    }
    saveToPath( currentFilePath_ );
}

void SMainWindow::fileSaveAs()
{
    if( !currentProject_ ) return;

    QString startDir = currentFilePath_.isEmpty()
        ? SSettings::instance().lastDir( "project", QDir::currentPath() )
        : currentFilePath_;

    QString fileName(
        QFileDialog::getSaveFileName(
            this,
            "Save Project As",
            startDir,
            "qbx Projects (*.qxp)" ) );
    if( fileName.isNull() ) {
        return;  // user cancelled
    }

    // Ensure the .qxp extension so Open's filter finds it later.
    if( !fileName.endsWith( ".qxp", Qt::CaseInsensitive ) ) {
        fileName += ".qxp";
    }

    saveToPath( fileName );
}

void SMainWindow::fileClose()
{
    closeProject();
    currentFilePath_.clear();
    setCentralWidget( NULL );   // drop the (now-deleted) project widget
    projectRootWidget_ = NULL;
    updateWindowTitle();
}

void SMainWindow::updateWindowTitle()
{
    QString name = currentProject_
        ? ( currentFilePath_.isEmpty()
                ? QString( "untitled" )
                : QFileInfo( currentFilePath_ ).fileName() )
        : QString();

    setWindowTitle( name.isEmpty() ? QString( "Smaragd" )
                                   : QString( "Smaragd - %1" ).arg( name ) );
}

void SMainWindow::fileNew()
{
    // FIXME: Delete the old component
    closeProject();

    currentProject_ = new SProject();

    createDocksToolbars();    
    // (void) efl;
    // qTBExternFileList_->show();

    // Create default main component for mixing. Could instantiate a wave view later here.
    currentProject_->setRootComponent( new SStdMixer( currentProject_ ) );    
    // Find out the main widget.
    // We do have a root component here as we assigned it before.
    projectRootWidget_ = currentProject_->getRootComponent()->getDetailEditWidget( this );

    setCentralWidget( projectRootWidget_ );
    projectRootWidget_->show();
    SApplication::app().setCurrentProject( currentProject_ );

    currentFilePath_.clear();   // fresh project is untitled until saved
    updateWindowTitle();
}

void SMainWindow::fileOpen()
{
    // FIXME: Delete the old component
    closeProject();
//    newProject();
    QString fileName(
        QFileDialog::getOpenFileName(
            this,
            "Open Project",
            SSettings::instance().lastDir( "project", QDir::currentPath() ),
            "qbx Projects (*.qxp *.QXP)" ) );
    if( fileName.isNull() ) {
        qWarning( "Nothing selected in file requester.\n" );
        return;
    }
    SSettings::instance().setLastDir( "project",
                                      QFileInfo( fileName ).absolutePath() );

    // Now, as the reading proceeded, create an empty project to fill in the data.
    currentProject_ = new SProject();
    SApplication::app().setCurrentProject( currentProject_ );

    createDocksToolbars();

    // Load via the same action a script or round-trip test would use.
    SLoadProjectAction action( fileName );
    SApplyResult r = action.apply( currentProject_ );
    if( !r.applied ) {
        QMessageBox::information( nullptr, "Smaragd warning",
                                  "Unable to open specified project file.",
                                  QMessageBox::Ok );
        closeProject();   // discard the half-built placeholder project
        updateWindowTitle();
        return;   // currentProject_ is gone; do not touch it below
    }

    // Find out the main widget.
    // We do have a root component here as we assigned it before.
    projectRootWidget_ = currentProject_->getRootComponent()->getDetailEditWidget( this );

    setCentralWidget( projectRootWidget_ );
    projectRootWidget_->show();
    SApplication::app().setCurrentProject( currentProject_ );

    currentFilePath_ = fileName;   // remember where we loaded from
    updateWindowTitle();
}

void SMainWindow::fileExit()
{
    ::exit( 0 );
}

void SMainWindow::newProject()
{
#if 0
    currentProject_ = new SProject();
    projectRootWidget_ = currentProject_->getRootComponent()->getDetailEditWidget( this );
    setCentralWidget( projectRootWidget_ );
    SApplication::app().setCurrentProject( currentProject_ );
    qTBExternFileList = new QToolBar( "Extern file list", this, Left );
    SExternFileList *efl = new SExternFileList( qTBExternFileList, *currentProject_ );
    qTBExternFileList->show();
#endif
}

void SMainWindow::closeProject()
{
    if( !currentProject_ ) return;
    SApplication::app().setCurrentProject( NULL );
    delete projectRootWidget_;
    projectRootWidget_ = NULL;   // avoid a dangling pointer / double-free
    delete currentProject_;
    destroyDocksToolbars();
    currentProject_ = NULL;
}

void SMainWindow::startPlaying()
{
    qWarning() << "startPlaying(): Called." << Qt::endl;
    if( !currentProject_ ) return;
    if( SApplication::app().isPlaying() ) {
        qWarning() << "startPlaying(): Ought to stop." << Qt::endl;
        SApplication::app().getSpeaker()->stopOutput();        
        actPlay_->setIcon( QIcon( QPixmap( (const char **)playoff_xpm ) ) );
        SApplication::app().setPlaying( false );
    } else {
        qWarning() << "startPlaying(): Ought to start." << Qt::endl;
        // FIXME: Add myselves as listener of the root component.
        SObject *root = currentProject_->getRootComponent();
        if( !root ) return;
        qWarning() << "startPlaying(): Preparing start." << Qt::endl;
        qWarning() << "startPlaying(): About to call root->seekTo()" << Qt::endl;
        root->seekTo( SApplication::app().getGlobalLocatorPos() );
        qWarning() << "startPlaying(): After root->seekTo()" << Qt::endl;
        qWarning() << "startPlaying(): About to call getSpeaker()->startOutput()" << Qt::endl;
        SApplication::app().getSpeaker()->startOutput();
        qWarning() << "startPlaying(): After getSpeaker()->startOutput()" << Qt::endl;
        actPlay_->setIcon( QIcon( QPixmap( (const char **)playon_xpm ) ) );
        SApplication::app().setPlaying( true );
    }
} 

void SMainWindow::stopPlaying()
{
    if( !currentProject_ ) return;
    if( SApplication::app().isPlaying() ) {
        SApplication::app().getSpeaker()->stopOutput();
        SApplication::app().setPlaying( false );
    } else {
        SObject *root = currentProject_->getRootComponent();
        if( !root ) return;        
        // FIXME: Jump to left locator here.
        SApplication::app().setGlobalLocatorPos( 0 );
        //  currentProject_->getRootComponent().seekTo( 0 );
    }
    actPlay_->setIcon( QIcon( QPixmap( (const char **)playoff_xpm ) ) );
}

SMainWindow::SMainWindow()
    : QMainWindow(),
      currentProject_( 0 ),
      projectRootWidget_( NULL )
{
    fprintf(stderr, "*** SMainWindow built on %s at %s ***\n", __DATE__, __TIME__);
    fflush(stderr);

    actPlay_ = new QAction( 
        QIcon( QPixmap( (const char **)playoff_xpm )),
        "Start playing",
        this);
    actPlay_->setShortcut(Qt::Key_Space);
    // actPlay_->setMenuText("Start");

    actStop_ = new QAction(
        QIcon( QPixmap( (const char **)stopoff_xpm )),
        "Stop playing",
        this);
    actStop_->setShortcut(Qt::Key_0);
    // actStop_->setMenuText("Stop");
    /*new QAction( "Stop playing", QIcon( QPixmap( "images/player_stop.png" ) ),
                            "Stop", Qt::Key_0, this );*/
    qTBTransport_ = new QToolBar( "Transport" /*, this*/ );
    /*
    actPlay_->addTo( qTBTransport );
    actStop_->addTo( qTBTransport );
    */
    qTBTransport_->addAction( actPlay_ );
    qTBTransport_->addAction( actStop_ );
    addToolBar( Qt::TopToolBarArea, qTBTransport_ );

    QObject::connect( actPlay_, SIGNAL( triggered() ), 
                      this, SLOT( startPlaying() ) );
    QObject::connect( actStop_, SIGNAL( triggered() ), 
                      this, SLOT( stopPlaying() ) );

    qFileMenu_ = new QMenu( "&File", this );
    qFileMenu_->setTearOffEnabled(true);
    qFileMenu_->addAction( "&New...", Qt::CTRL | Qt::Key_N, this, SLOT( fileNew() ) );
    qFileMenu_->addAction( "&Open...", this, SLOT( fileOpen() ) );
    qFileMenu_->addAction( "&Save", Qt::CTRL | Qt::Key_S, this, SLOT( fileSave() ) );
    qFileMenu_->addAction( "Save &as...", Qt::CTRL | Qt::SHIFT | Qt::Key_S, this, SLOT( fileSaveAs() ) );
    qFileMenu_->addSeparator();
    qFileMenu_->addAction( "&Close", Qt::CTRL | Qt::Key_W, this, SLOT( fileClose() ) );
    qFileMenu_->addSeparator();
    qFileMenu_->addAction( "E&xit", Qt::CTRL | Qt::Key_Q, this, SLOT( fileExit() ) );
    menuBar()->addMenu( qFileMenu_ );

    QMenu *editMenu = new QMenu( "&Edit", this );
    editMenu->addAction( "&Undo", Qt::CTRL | Qt::Key_Z, this, SLOT( undo() ) );
    editMenu->addAction( "&Redo", Qt::CTRL | Qt::SHIFT | Qt::Key_Z, this, SLOT( redo() ) );
    menuBar()->addMenu( editMenu );

    buildAudioMenu();

    qTestMenu_ = new QMenu( "&Test", this );
    qTestMenu_->setTearOffEnabled(true);
    qTestMenu_->addAction( "&Run Test Sequence...", this, SLOT( runTestSequence() ) );
    qTestMenu_->addAction( "&Volume Burst (track 0)", this, SLOT( runVolumeBurst() ) );
    qTestMenu_->addAction( "Save/&Load Round-trip", this, SLOT( runSaveLoadTest() ) );
    menuBar()->addMenu( qTestMenu_ );

    qDockExternFileList_ = NULL;
}

void SMainWindow::buildAudioMenu()
{
    qAudioMenu_ = new QMenu( "&Audio", this );
    QMenu *devMenu = qAudioMenu_->addMenu( "Output &Device" );

    deviceGroup_ = new QActionGroup( this );
    deviceGroup_->setExclusive( true );

    twSpeaker *spk = SApplication::app().getSpeaker();
    const std::string current = spk->outputDevice();
    std::vector<audio::AudioDeviceInfo> devs = spk->outputDevices();

    auto addDevice = [&]( const QString &label, const QString &id ) {
        QAction *a = devMenu->addAction( label );
        a->setCheckable( true );
        a->setData( id );
        a->setChecked( id.toStdString() == current );
        deviceGroup_->addAction( a );
    };

    if( devs.empty() ) {
        // Backend offers no enumeration (e.g. NullBackend): just the default.
        addDevice( "System default", "default" );
    } else {
        for( const audio::AudioDeviceInfo &d : devs )
            addDevice( QString::fromStdString( d.name ),
                       QString::fromStdString( d.id ) );
    }

    // If the saved device is gone, fall back to checking the first entry.
    if( !deviceGroup_->checkedAction() && !deviceGroup_->actions().isEmpty() )
        deviceGroup_->actions().first()->setChecked( true );

    connect( deviceGroup_, &QActionGroup::triggered,
             this, &SMainWindow::audioDeviceSelected );

    menuBar()->addMenu( qAudioMenu_ );
}

void SMainWindow::audioDeviceSelected( QAction *a )
{
    if( !a ) return;
    const QString id = a->data().toString();
    SApplication::app().getSpeaker()->setOutputDevice( id.toStdString() );
    SSettings::instance().setAudioDeviceId( id );
    if( SApplication::app().isPlaying() )
        statusBar()->showMessage(
            "Audio device change takes effect on the next Play.", 4000 );
}

void SMainWindow::runTestSequence()
{
    fprintf(stderr, "runTestSequence() CALLED\n");
    fflush(stderr);

    // Open file dialog to pick a WAV file.
    fprintf(stderr, "  Opening file dialog...\n");
    fflush(stderr);
    QString lastDir = SSettings::instance().lastDir("sample", "");
    QFileDialog dialog(this, "Select a WAV file for the test sequence", lastDir, "WAV Files (*.wav);;All Files (*)");
    dialog.setOptions(QFileDialog::DontUseNativeDialog);
    QString filePath;
    if (dialog.exec() == QDialog::Accepted) {
        filePath = dialog.selectedFiles().first();
    }
    fprintf(stderr, "  File dialog closed. filePath=%s\n", filePath.toStdString().c_str());
    fflush(stderr);

    if (filePath.isEmpty()) {
        fprintf(stderr, "  User cancelled. Returning.\n");
        fflush(stderr);
        return;
    }

    // Save the directory.
    QFileInfo fileInfo(filePath);
    SSettings::instance().setLastDir("sample", fileInfo.absolutePath());

    // Create a new project.
    fprintf(stderr, "  Creating new project...\n");
    fflush(stderr);
    fileNew();

    if (!currentProject_) {
        fprintf(stderr, "  ERROR: Failed to create new project!\n");
        fflush(stderr);
        QMessageBox::critical(this, "Error", "Failed to create new project");
        return;
    }

    fprintf(stderr, "  Submitting actions...\n");
    fflush(stderr);
    // Submit the test sequence actions.
    // 1. Add a track at index 0.
    SApplication::app().submitAction(new SAddTrackAction(0));
    fprintf(stderr, "    Add track action submitted\n");
    fflush(stderr);

    // 2. Add the sample to track 0 at time 0.
    SApplication::app().submitAction(new SAddSampleAction(0, filePath, 0));
    fprintf(stderr, "    Add sample action submitted\n");
    fflush(stderr);

    // 3. Start playback.
    SApplication::app().submitAction(new STogglePlaybackAction(true));
    fprintf(stderr, "    Toggle playback action submitted\n");
    fflush(stderr);

    statusBar()->showMessage("Test sequence started", 2000);
    fprintf(stderr, "  Test sequence complete!\n");
    fflush(stderr);
}

// Phase 2b validation: fire a rapid burst of volume changes at track 0 and
// confirm (a) they are audible while playing, (b) the undo stack collapses the
// burst to a single entry via mergeKey()/mergeWith(), and (c) one undo restores
// the pre-burst level. Run "Run Test Sequence..." first so track 0 has audio.
void SMainWindow::runVolumeBurst()
{
    if (!currentProject_) {
        statusBar()->showMessage("Run the Test Sequence first (need a track to drive)", 3000);
        return;
    }

    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    int before = stack ? stack->count() : -1;

    // Ramp -24 db .. +6 db over 50 steps, all on track 0. In the current
    // synchronous drain model each one applies immediately, but the QUndoStack
    // merges them (same mergeKey) into a single undo entry.
    const int steps = 50;
    for (int i = 0; i < steps; ++i) {
        double db = -24.0 + (30.0 * i) / (steps - 1);
        SApplication::app().submitAction(new SSetTrackVolumeAction(0, db));
    }

    int after = stack ? stack->count() : -1;
    fprintf(stderr, "Volume burst: %d actions submitted; undo stack %d -> %d "
                    "(expect +1 if merge worked)\n", steps, before, after);
    fflush(stderr);
    statusBar()->showMessage(
        QString("Volume burst: %1 actions -> undo stack +%2 (expect +1)")
            .arg(steps).arg(after - before), 4000);
}

// Save/load validation: drive SSaveProjectAction + SLoadProjectAction (the same
// actions the File menu uses) as a self-contained assertion. Saves the live
// project to a temp file, reloads it into a throwaway project, and compares
// track counts. The live project is never disturbed.
void SMainWindow::runSaveLoadTest()
{
    if (!currentProject_) {
        statusBar()->showMessage("Open or create a project first", 3000);
        return;
    }

    SStdMixer *liveMixer = dynamic_cast<SStdMixer*>(currentProject_->getRootComponent());
    int liveTracks = liveMixer ? liveMixer->getNTracks() : -1;

    QString tmpPath = QDir::tempPath() + "/smaragd_roundtrip.qxp";

    // 1. Save the live project via the action.
    SSaveProjectAction saveAction(tmpPath);
    if (!saveAction.apply(currentProject_).applied) {
        statusBar()->showMessage("Round-trip FAILED: save error", 5000);
        return;
    }

    // 2. Reload into a throwaway project via the action (live project untouched).
    SProject *probe = new SProject();
    bool loaded = SLoadProjectAction(tmpPath).apply(probe).applied;

    int probeTracks = -1;
    if (loaded) {
        SStdMixer *probeMixer = dynamic_cast<SStdMixer*>(probe->getRootComponent());
        probeTracks = probeMixer ? probeMixer->getNTracks() : -1;
    }
    delete probe;

    bool ok = loaded && (probeTracks == liveTracks);
    QString msg = ok
        ? QString("Round-trip OK: %1 tracks saved and reloaded").arg(liveTracks)
        : QString("Round-trip FAILED: live=%1 reloaded=%2 (loaded=%3)")
              .arg(liveTracks).arg(probeTracks).arg(loaded ? "yes" : "no");
    fprintf(stderr, "%s  [%s]\n", msg.toUtf8().constData(), tmpPath.toUtf8().constData());
    fflush(stderr);
    statusBar()->showMessage(msg, 5000);
}

void SMainWindow::undo()
{
    SApplication::app().actionHistory()->undo();
    // Refresh the view after undo
    if (projectRootWidget_) {
        projectRootWidget_->update();
    }
}

void SMainWindow::redo()
{
    SApplication::app().actionHistory()->redo();
    // Refresh the view after redo
    if (projectRootWidget_) {
        projectRootWidget_->update();
    }
}

SMainWindow::~SMainWindow()
{
}

