
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
#include <QStandardPaths>
#include <QStatusBar>
#include <QLabel>
#include <QInputDialog>
#include <QTimer>
#include <QVBoxLayout>
#include <QCloseEvent>

#include <iostream>

#include "app/shell/sapplication.h"
#include "app/timeline/sgridtoolbar.h"
#include "app/shell/smainwindow.h"
#include "app/model/sobject.h"
#include "app/model/sproject.h"
#include "app/shell/ssettings.h"
#include "app/servicesui/srecordingprogress.h"
#include "app/objects/cut/scut.h"
#include "app/objects/wave/splainwave.h"
#include "app/model/slink.h"
#include "app/objects/track/strack.h"
#include "tw/record/recording_session.h"
#include <QFileInfo>
#include "app/model/sprojectprops.h"
#include "app/model/sexternfilelist.h"
#include "app/model/slink.h"
#include "app/objects/cut/scut.h"
#include "app/persistence/sprojectloader.h"
#include "app/servicesui/srenderdialog.h"
#include "app/servicesui/srenderprogress.h"
#include "app/shell/ssettings.h"
#include "app/actions/sactionhistory.h"
#include <QUndoStack>

#include "app/objects/mixer/sstdmixer.h"
#include "app/timeline/sstdmixerview.h"
#include "app/objects/track/strack.h"
#include "app/servicesui/soptionsdialog.h"

#include "tw/playback/twspeaker.h"
#include "tw/devices/audio_input.h"

#include "app/objects/track/saddtrackaction.h"
#include "app/objects/track/sreparenttrackaction.h"
#include "app/objects/track/smovetrackaction.h"
#include "app/objects/track/sremovetrackaction.h"
#include "app/objects/wave/saddsampleaction.h"
#include "app/objects/track/ssettrackvolumeaction.h"
#include "app/persistence/ssaveprojectaction.h"
#include "app/actions/sloadprojectaction.h"
#include "app/actions/ssnaptogridaction.h"
#include "app/actions/sgridaction.h"
#include "app/actions/smetronomeaction.h"
#include "app/actions/scycleaction.h"
#include "app/actions/stoggleplaybackaction.h"

#include <QPixmap>
#include <QPainter>
#include <QIcon>
#include <QFont>
#include <QKeySequence>

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
    // Clear the extern file list when closing the project.
    if( externFileList_ ) {
        externFileList_->setProject( nullptr );
    }
}

void SMainWindow::createDocksToolbars()
{
    // Populate the extern file list with the current project's files and assets.
    if( externFileList_ && currentProject_ ) {
        externFileList_->setProject( currentProject_ );
    }
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
    SSettings::instance().addRecentProject( path );
    updateRecentMenu();
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

    QString startDir;
    if( !currentFilePath_.isEmpty() ) {
        startDir = currentFilePath_;
    } else {
        startDir = SSettings::instance().lastDir( "project", QString() );
        if( startDir.isEmpty() ) {
            startDir = QStandardPaths::writableLocation( QStandardPaths::DocumentsLocation )
                     + QDir::separator() + "smaragd";
            QDir().mkpath( startDir );
        }
    }

    QFileDialog dialog( this, "Save Project As", startDir, "qbx Projects (*.qxp)" );
    dialog.setFileMode( QFileDialog::AnyFile );
    dialog.setAcceptMode( QFileDialog::AcceptSave );
    dialog.setOptions( QFileDialog::DontUseNativeDialog );
    QString fileName;
    if( dialog.exec() == QDialog::Accepted ) {
        fileName = dialog.selectedFiles().isEmpty() ? QString() : dialog.selectedFiles().at( 0 );
    }
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
    if( !promptSaveUnsavedChanges() ) return;  // User cancelled

    closeProject();             // deletes the project (auto-removes its connections)
    currentFilePath_.clear();
    setCentralWidget( NULL );   // drop the (now-deleted) project widget
    projectRootWidget_ = NULL;
    updateWindowTitle();
    syncPaletteToProject( NULL );
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
    if( !promptSaveUnsavedChanges() ) return;  // User cancelled
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
    syncPaletteToProject( currentProject_ );
}

void SMainWindow::fileOpen()
{
    QString defaultDir = SSettings::instance().lastDir( "project", QString() );
    if( defaultDir.isEmpty() ) {
        defaultDir = QStandardPaths::writableLocation( QStandardPaths::DocumentsLocation )
                   + QDir::separator() + "smaragd";
        QDir().mkpath( defaultDir );
    }
    QFileDialog dialog( this, "Open Project", defaultDir, "qbx Projects (*.qxp *.QXP)" );
    dialog.setFileMode( QFileDialog::ExistingFile );
    dialog.setOptions( QFileDialog::DontUseNativeDialog );
    QString fileName;
    if( dialog.exec() == QDialog::Accepted ) {
        fileName = dialog.selectedFiles().isEmpty() ? QString() : dialog.selectedFiles().at( 0 );
    }
    if( fileName.isNull() ) {
        qWarning( "Nothing selected in file requester.\n" );
        return;   // user cancelled: keep the current project untouched
    }
    openProjectFile( fileName );
}

bool SMainWindow::openProjectFile( const QString &fileName )
{
    if( fileName.isEmpty() ) return false;

    // FIXME: Delete the old component
    if( !promptSaveUnsavedChanges() ) return false;  // User cancelled
    closeProject();

    SSettings::instance().setLastDir( "project",
                                      QFileInfo( fileName ).absolutePath() );

    // Create an empty project to fill in the data as the reading proceeds.
    currentProject_ = new SProject();
    SApplication::app().setCurrentProject( currentProject_ );

    // Load via the same action a script or round-trip test would use.
    SLoadProjectAction action( fileName );
    SApplyResult r = action.apply( currentProject_ );
    if( !r.applied ) {
        QMessageBox::information( nullptr, "Smaragd warning",
                                  "Unable to open specified project file.",
                                  QMessageBox::Ok );
        // Failed load — mark as partial so destructor skips unsafe cleanup,
        // then use deleteLater() to safely let Qt clean up child objects.
        SApplication::app().setCurrentProject( NULL );
        currentProject_->markAsPartialLoad();
        currentProject_->deleteLater();
        currentProject_ = NULL;
        projectRootWidget_ = NULL;
        updateWindowTitle();
        return false;
    }

    // Load succeeded; now create UI elements (docks/toolbars) that reference the project
    createDocksToolbars();

    // Find out the main widget.
    // We do have a root component here as we assigned it before.
    projectRootWidget_ = currentProject_->getRootComponent()->getDetailEditWidget( this );

    setCentralWidget( projectRootWidget_ );
    projectRootWidget_->show();
    SApplication::app().setCurrentProject( currentProject_ );

    currentFilePath_ = fileName;   // remember where we loaded from
    updateWindowTitle();
    syncPaletteToProject( currentProject_ );

    SSettings::instance().addRecentProject( fileName );
    updateRecentMenu();
    return true;
}

void SMainWindow::updateRecentMenu()
{
    if( !qRecentMenu_ ) return;
    qRecentMenu_->clear();

    const QStringList recents = SSettings::instance().recentProjects();
    if( recents.isEmpty() ) {
        QAction *a = qRecentMenu_->addAction( "(none)" );
        a->setEnabled( false );
        return;
    }

    int n = 1;
    for( const QString &path : recents ) {
        const QString label =
            QString( "&%1  %2" ).arg( n++ ).arg( QFileInfo( path ).fileName() );
        QAction *a = qRecentMenu_->addAction( label );
        a->setData( path );
        a->setToolTip( path );
        connect( a, &QAction::triggered, this, [this, path] {
            if( !openProjectFile( path ) ) {
                // File gone / unreadable — drop it so the list stays useful.
                SSettings::instance().removeRecentProject( path );
                updateRecentMenu();
            }
        } );
    }
}

bool SMainWindow::restoreWindowLayout()
{
    const QByteArray geo = SSettings::instance().windowGeometry();
    const bool geoRestored = !geo.isEmpty() && restoreGeometry( geo );
    const QByteArray state = SSettings::instance().windowState();
    if( !state.isEmpty() ) restoreState( state );
    return geoRestored;
}

void SMainWindow::openMostRecent()
{
    const QStringList recents = SSettings::instance().recentProjects();
    for( const QString &path : recents ) {
        if( !QFileInfo::exists( path ) ) {
            // Missing on disk: forget it and try the next-newest.
            SSettings::instance().removeRecentProject( path );
            continue;
        }
        // File exists; try to open it
        if( openProjectFile( path ) )
            return;  // Success, we're done
        // File exists but failed to open (corrupted, etc.); remove it and try next
        SSettings::instance().removeRecentProject( path );
    }
    // Nothing to restore — start empty (File → New to begin a session).
    updateRecentMenu();
}

void SMainWindow::fileExit()
{
    ::exit( 0 );
}

void SMainWindow::onRenderTriggered()
{
    SProject *project = SApplication::app().getCurrentProject();
    if (!project) {
        QMessageBox::warning(this, "No Project", "Please open or create a project first.");
        return;
    }

    // Show render dialog
    SRenderDialog dialog(project, this);
    if (dialog.exec() == QDialog::Accepted) {
        audio::RenderParams params = dialog.getRenderParams();

        // Start render first (creates/resets the RenderSession)
        SApplication::app().startRender(params);

        // Show progress dialog and start rendering
        SRenderProgressDialog *progressDialog = new SRenderProgressDialog(
            SApplication::app().renderSession(),
            QString::fromStdString(params.outputPath), this);

        // Set dialog to auto-delete when closed
        progressDialog->setAttribute(Qt::WA_DeleteOnClose);

        // Show the dialog (will be modal-like, but allows event processing)
        progressDialog->exec();  // Block until user closes the dialog
    }
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
    // Stop playback before destroying the project to prevent audio thread access
    if( SApplication::app().isPlaying() ) {
        stopPlaying();
    }
    SApplication::app().setCurrentProject( NULL );
    destroyDocksToolbars();
    // Detach (not delete) projectRootWidget_ from the main window so it doesn't
    // interfere with project destruction. The project may have created children
    // that are part of its QObject tree; deleting them here would corrupt the
    // tree before the project destructor runs.
    setCentralWidget( nullptr );
    projectRootWidget_ = NULL;
    delete currentProject_;
    currentProject_ = NULL;
}

bool SMainWindow::hasUnsavedChanges() const
{
    if( !currentProject_ ) return false;

    // Check if the undo stack is "clean" (no unsaved changes)
    QUndoStack *stack = SApplication::app().actionHistory()->undoStack();
    return !stack->isClean();
}

bool SMainWindow::promptSaveUnsavedChanges()
{
    if( !hasUnsavedChanges() ) return true;  // No unsaved changes, proceed

    // Determine project name for the dialog
    QString projectName = currentFilePath_.isEmpty() ? "Untitled" :
                         QFileInfo( currentFilePath_ ).fileName();

    QMessageBox msgBox( this );
    msgBox.setWindowTitle( "Unsaved Changes" );
    msgBox.setText( QString( "Unsaved work in project \"%1\"" ).arg( projectName ) );
    msgBox.setStandardButtons( QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel );
    msgBox.setDefaultButton( QMessageBox::Save );
    msgBox.setIcon( QMessageBox::Warning );

    int result = msgBox.exec();

    switch( result ) {
        case QMessageBox::Save:
            fileSave();  // This will save to currentFilePath_ or prompt Save As if untitled
            return true;
        case QMessageBox::Discard:
            return true;
        case QMessageBox::Cancel:
        default:
            return false;
    }
}

void SMainWindow::closeEvent( QCloseEvent *event )
{
    if( promptSaveUnsavedChanges() ) {
        // Save window geometry and toolbar/dock state while the full UI —
        // including the project's central widget — still exists. Saving after
        // closeProject() records a layout without a central widget, which does
        // not round-trip through restoreState().
        SSettings::instance().setWindowGeometry( saveGeometry() );
        SSettings::instance().setWindowState( saveState() );
        closeProject();
        // Ensure all settings are written to disk before exit
        SSettings::instance().value( "dummy" );  // Triggers internal sync
        event->accept();
    } else {
        event->ignore();
    }
}

void SMainWindow::postHint( const QString &text, int durationMs )
{
    statusBar()->showMessage( text, durationMs );
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
        // Arm cycle (loop) playback from the current project state before output
        // starts, so the loop region is honoured from the first buffer.
        syncCyclePlayback();

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

// Jump the global position to the start of the time range, if one is set,
// otherwise to the very beginning (zero).
void SMainWindow::gotoRangeStart()
{
    if( !currentProject_ ) return;

    offset_t pos = 0;
    if( currentProject_->hasTimeSelection() ) {
        SProject::TimeRange sel = currentProject_->getTimeSelection();
        double sampleRate = currentProject_->getSRate();
        pos = (offset_t)( sel.startSeconds * sampleRate );
    }

    SApplication::app().setGlobalLocatorPos( pos );
}

void SMainWindow::onRecordTriggered()
{
    if( !currentProject_ ) return;

    if( SApplication::app().isRecordingActive() ) {
        // Stop recording
        audio::RecordingSession *session = SApplication::app().recordingSession();
        if( session ) {
            session->requestStop();
        }
        actRecord_->setIcon( QIcon( QPixmap( (const char **)playoff_xpm ) ) );
    } else {
        // Check if any tracks are armed
        bool anyArmed = false;
        for( SLink *trackLink : currentProject_->getRootComponent()->childLinks() ) {
            if( trackLink->getSObject().isArmedForRecording() ) {
                anyArmed = true;
                break;
            }
        }

        if( !anyArmed ) {
            // Inform user to arm a track
            QMessageBox::information( this, "No Tracks Armed",
                "Please arm at least one track for recording before starting." );
            return;
        }

        // Start recording
        audio::RecordingParams params;
        // Get input device from settings (defaults to "default" if not set)
        QString inputDevId = SSettings::instance().audioInputDeviceId();
        params.inputDeviceId = inputDevId.toStdString();
        // Use the project file directory for recordings, or a default if unsaved
        QString projectDir = currentFilePath_.isEmpty() ?
            QStandardPaths::writableLocation( QStandardPaths::DocumentsLocation ) :
            QFileInfo( currentFilePath_ ).absolutePath();
        params.projectDirectory = projectDir.toStdString();
        params.sampleRate = currentProject_->getSRate();
        params.channels = 2;

        // Collect armed track IDs, STrack pointers, and per-track channel selections
        std::vector<STrack*> armedTracks;
        for( SLink *trackLink : currentProject_->getRootComponent()->childLinks() ) {
            if( trackLink->getSObject().isArmedForRecording() ) {
                params.armedTrackIds.push_back( trackLink->getSObject().getSName().toStdString() );
                params.trackChannels.push_back( trackLink->getSObject().getRecordingChannels() );
                if( STrack *track = dynamic_cast<STrack*>( &trackLink->getSObject() ) ) {
                    armedTracks.push_back( track );
                }
            }
        }

        // Remember where the playhead is now: the cut goes here, and the playhead
        // advances from here during the capture.
        recordingStartPos_ = SApplication::app().getGlobalLocatorPos();

        // Note: latency sync offset will be calculated in onRecordingCompleted()
        // after the input latency is known from the recording session.
        recordingLatencySyncOffset_ = 0;

        SApplication::app().startRecording( params );
        actRecord_->setIcon( QIcon( QPixmap( (const char **)playon_xpm ) ) );

        // Show recording progress dialog
        audio::RecordingSession *recSession = SApplication::app().recordingSession();
        if( recSession ) {
            recordingProgressDialog_ = new SRecordingProgressDialog( recSession, this );
            int result = recordingProgressDialog_->exec();

            // Recording has ended: stop the monitoring playback we started in
            // startRecording (safe now that the audio thread never touches Qt).
            if( SApplication::app().isPlaying() ) {
                SApplication::app().getSpeaker()->stopOutput();
                SApplication::app().setPlaying( false );
            }

            // On dialog close, place the cuts on armed tracks
            if( result == QDialog::Accepted ) {
                onRecordingCompleted();
            }
        }
    }
}

void SMainWindow::onRecordingCompleted()
{
    if( !currentProject_ ) return;

    audio::RecordingSession *recSession = SApplication::app().recordingSession();
    if( !recSession ) return;

    // Get the created files (one per armed track)
    const auto &createdFiles = recSession->createdFiles();
    if( createdFiles.empty() ) return;

    // The recording start time, captured when recording began (the live locator
    // has since advanced with the capture).
    offset_t recordingStartTime = recordingStartPos_;

    // Calculate latency sync offset if playback was running during recording.
    // Offset = output_latency - input_latency (in frames).
    // Positive offset: input is faster, so shift the clip earlier to compensate.
    int64_t latencySyncFrames = 0;
    twSpeaker *speaker = SApplication::app().getSpeaker();
    if( speaker ) {
        audio::AudioBackend *backend = speaker->getBackend();
        uint32_t inputLatency = recSession->getInputLatencyFrames();
        if( backend && inputLatency > 0 ) {
            uint32_t outputLatency = backend->getLatencyFrames();
            latencySyncFrames = static_cast<int64_t>(outputLatency) - static_cast<int64_t>(inputLatency);
        }
    }

    // Apply the offset to the recording start position
    if( latencySyncFrames != 0 ) {
        // latencySyncFrames is in samples; convert to the timeline representation
        recordingStartTime += latencySyncFrames;
    }

    // Place cuts on all armed tracks, using the corresponding per-track WAV file
    int fileIndex = 0;
    for( SLink *trackLink : currentProject_->getRootComponent()->childLinks() ) {
        if( !trackLink->getSObject().isArmedForRecording() ) continue;

        STrack *track = dynamic_cast<STrack*>( &trackLink->getSObject() );
        if( !track ) continue;

        // Get the corresponding WAV file for this track
        if( fileIndex >= (int)createdFiles.size() ) {
            // No file for this track (shouldn't happen, but safety check)
            track->setArmedForRecording( false );
            fileIndex++;
            continue;
        }

        QString recordedFile = QString::fromStdString( createdFiles[fileIndex] );
        if( !QFileInfo( recordedFile ).exists() ) {
            track->setArmedForRecording( false );
            fileIndex++;
            continue;
        }

        // Create a SPlainWave for the recorded file
        SPlainWave *wave = new SPlainWave( currentProject_ );
        if( wave->setWave( recordedFile ) < 0 ) {
            delete wave;
            track->setArmedForRecording( false );
            fileIndex++;
            continue;
        }

        // Create a SCut wrapping the wave
        SCut *cut = new SCut( currentProject_, *wave );

        // Create an SLink to place the cut in the track
        SLink *link = new SLink( *cut, nullptr );
        link->setStartTime( recordingStartTime );
        link->setParent( track );

        // Mark track as no longer armed
        track->setArmedForRecording( false );
        fileIndex++;
    }

    // Return the playhead to where recording began, lining it up with the cut we
    // just placed (it had advanced to the end during capture).
    SApplication::app().setGlobalLocatorPos( recordingStartTime );

    // Refresh the UI to display the newly placed clip
    if( projectRootWidget_ ) {
        projectRootWidget_->update();
    }

    actRecord_->setIcon( QIcon( QPixmap( (const char **)playoff_xpm ) ) );
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
    // actStop_->setMenuText("Stop");
    /*new QAction( "Stop playing", QIcon( QPixmap( "images/player_stop.png" ) ),
                            "Stop", Qt::Key_0, this );*/

    // "0" jumps the global position to the start of the time range
    // (if one is set), otherwise to zero.
    actGotoStart_ = new QAction( "Go to range start", this );
    actGotoStart_->setShortcut( Qt::Key_0 );

    actRecord_ = new QAction(
        QIcon( QPixmap( (const char **)playoff_xpm )),
        "Record",
        this);
    // Set keyboard shortcuts: Ctrl-R (Windows/Linux), Cmd-R (macOS), and numpad *
    actRecord_->setShortcut( Qt::CTRL | Qt::Key_R );
    // Note: macOS uses Cmd which Qt maps to Meta, but Qt::CTRL is more portable
    // To support Cmd-R on macOS, we could add: actRecord_->setShortcut( Qt::META | Qt::Key_R );
    // For now, Ctrl-R works on all platforms

    qTBTransport_ = new QToolBar( "Transport" /*, this*/ );
    qTBTransport_->setObjectName( "toolbar_transport" );
    /*
    actPlay_->addTo( qTBTransport );
    actStop_->addTo( qTBTransport );
    */
    qTBTransport_->addAction( actPlay_ );
    qTBTransport_->addAction( actStop_ );
    qTBTransport_->addAction( actRecord_ );
    addToolBar( Qt::TopToolBarArea, qTBTransport_ );

    QObject::connect( actPlay_, SIGNAL( triggered() ),
                      this, SLOT( startPlaying() ) );
    QObject::connect( actStop_, SIGNAL( triggered() ),
                      this, SLOT( stopPlaying() ) );
    QObject::connect( actRecord_, SIGNAL( triggered() ),
                      this, SLOT( onRecordTriggered() ) );

    // Register the goto-start action on the window so its "0" shortcut is
    // active even though it has no toolbar/menu entry.
    addAction( actGotoStart_ );
    QObject::connect( actGotoStart_, SIGNAL( triggered() ),
                      this, SLOT( gotoRangeStart() ) );

    qFileMenu_ = new QMenu( "&File", this );
    qFileMenu_->setTearOffEnabled(true);
    qFileMenu_->addAction( "&New...", Qt::CTRL | Qt::Key_N, this, SLOT( fileNew() ) );
    qFileMenu_->addAction( "&Open...", this, SLOT( fileOpen() ) );
    qRecentMenu_ = qFileMenu_->addMenu( "Open &Recent" );
    updateRecentMenu();
    qFileMenu_->addAction( "&Save", Qt::CTRL | Qt::Key_S, this, SLOT( fileSave() ) );
    qFileMenu_->addAction( "Save &as...", Qt::CTRL | Qt::SHIFT | Qt::Key_S, this, SLOT( fileSaveAs() ) );
    qFileMenu_->addSeparator();
    qFileMenu_->addAction( "&Render...", this, SLOT( onRenderTriggered() ) );
    qFileMenu_->addSeparator();
    qFileMenu_->addAction( "&Close", Qt::CTRL | Qt::Key_W, this, SLOT( fileClose() ) );
    qFileMenu_->addSeparator();
    qFileMenu_->addAction( "E&xit", Qt::CTRL | Qt::Key_Q, this, SLOT( fileExit() ) );
    menuBar()->addMenu( qFileMenu_ );

    QMenu *editMenu = new QMenu( "&Edit", this );
    editMenu->addAction( "&Undo", Qt::CTRL | Qt::Key_Z, this, SLOT( undo() ) );
    editMenu->addAction( "&Redo", Qt::CTRL | Qt::SHIFT | Qt::Key_Z, this, SLOT( redo() ) );
    editMenu->addSeparator();
    editMenu->addAction( "&Options...", QKeySequence( Qt::CTRL | Qt::Key_Comma ),
                         this, SLOT( showOptionsDialog() ) );
    menuBar()->addMenu( editMenu );

    buildAudioMenu();

    buildPaletteToolbar();

    qTestMenu_ = new QMenu( "&Test", this );
    qTestMenu_->setTearOffEnabled(true);
    qTestMenu_->addAction( "&Run Test Sequence...", this, SLOT( runTestSequence() ) );
    qTestMenu_->addAction( "&Volume Burst (track 0)", this, SLOT( runVolumeBurst() ) );
    qTestMenu_->addAction( "Test &Render...", this, SLOT( runTestRender() ) );
    qTestMenu_->addAction( "Set &Time Selection (first half)", this, SLOT( runSetTimeSelection() ) );
    qTestMenu_->addAction( "Save/&Load Round-trip", this, SLOT( runSaveLoadTest() ) );
    qTestMenu_->addAction( "&Group Track Test (tree + undo)", this, SLOT( runGroupTrackTest() ) );
    qTestMenu_->addAction( "Re&order Track Test (exact slot)", this, SLOT( runReorderTrackTest() ) );
    qTestMenu_->addAction( "&Nest Track 1 Under 0 (persist)", this, SLOT( runGroupPersist() ) );
    qTestMenu_->addAction( "Undoable Remo&ve Test (subtree)", this, SLOT( runUndoRemoveTest() ) );
    qTestMenu_->addSeparator();
    qTestMenu_->addAction( "Set Clip &Stretch... (selected)", this, SLOT( runSetClipStretch() ) );
    qTestMenu_->addAction( "Set Clip &Pitch... (selected)", this, SLOT( runSetClipPitch() ) );
    menuBar()->addMenu( qTestMenu_ );

    buildStatusBar();

    // Create the persistent extern file list dock. It outlives any single project;
    // its content is managed by setProject() called from createDocksToolbars.
    qDockExternFileList_ = new QDockWidget( tr( "Extern file list" ), this );
    qDockExternFileList_->setObjectName( "dock_extern_file_list" );
    externFileList_ = new SExternFileList( qDockExternFileList_, nullptr );
    qDockExternFileList_->setWidget( externFileList_ );
    addDockWidget( Qt::LeftDockWidgetArea, qDockExternFileList_ );

    // NOTE: window geometry/state restore deliberately does NOT happen here.
    // The saved state describes a window that includes the project's central
    // widget; restoring it before that widget exists (it is only created once
    // a project is opened) freezes the QMainWindow layout at the tiny pre-show
    // size. main() calls restoreWindowLayout() after openMostRecent().

    // Measure and cache audio device latencies on startup
    // (done after UI is built, will show modal dialog if needed)
    QTimer::singleShot( 100, this, &SMainWindow::measureAudioLatenciesIfNeeded );
}

// Build the status bar. The left area is used for transient showMessage()
// notices (saves, test results, …); the permanent right area carries a mode
// indicator that reflects the active editing gesture (slip, time-stretch, …).
void SMainWindow::buildStatusBar()
{
    modeLabel_ = new QLabel( this );
    modeLabel_->setMinimumWidth( 100 );
    statusBar()->addPermanentWidget( modeLabel_ );

    QObject::connect( &SApplication::app(), SIGNAL( statusModeChanged( const QString & ) ),
                      this, SLOT( onStatusModeChanged( const QString & ) ) );
    onStatusModeChanged( SApplication::app().getStatusMode() );
}

void SMainWindow::onStatusModeChanged( const QString &mode )
{
    if( !modeLabel_ ) return;
    modeLabel_->setText( mode );
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

void SMainWindow::runTestRender()
{
    if (!currentProject_) {
        statusBar()->showMessage("Create or open a project first", 3000);
        return;
    }

    // Open the render dialog
    onRenderTriggered();
}

void SMainWindow::runSetTimeSelection()
{
    if (!currentProject_) {
        statusBar()->showMessage("Create or open a project first", 3000);
        return;
    }

    // Set time selection to first half of project duration
    double duration = currentProject_->getDurationSeconds();
    currentProject_->setTimeSelection(0.0, duration / 2.0);
    statusBar()->showMessage(
        QString("Time selection set: 0.0 - %1 seconds").arg(duration / 2.0, 0, 'f', 2), 3000);
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

// Phase 2 (proposal 05 §1) validation: build a track tree with
// SReparentTrackAction, confirm the nested structure, round-trip it through
// save/load, and confirm undo restores the flat arrangement. Self-contained.
void SMainWindow::runGroupTrackTest()
{
    if (!currentProject_) {
        statusBar()->showMessage("Open or create a project first", 3000);
        return;
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(currentProject_->getRootComponent());
    if (!mixer) {
        statusBar()->showMessage("Group test FAILED: no mixer", 4000);
        return;
    }

    // Need at least two top-level tracks; add until we have two.
    while (mixer->getNTracks() < 2) {
        SApplication::app().submitAction(new SAddTrackAction(-1));
    }
    const int topBefore = mixer->getNTracks();

    // Count the track-typed children of a container (clips don't count).
    auto childTrackCount = [](SObject *container) {
        int n = 0;
        for (SLink *lk : container->childLinks()) {
            if (dynamic_cast<STrack*>(&lk->getSObject())) ++n;
        }
        return n;
    };

    SObject *topTrack0 = &mixer->childAt(0)->getSObject();
    const int nestedBefore = childTrackCount(topTrack0);

    // Move the second top-level track (path {1}) under the first (path {0}).
    SApplication::app().submitAction(
        new SReparentTrackAction(QList<int>{1}, QList<int>{0}));

    const int topAfter = mixer->getNTracks();
    const int nestedAfter = childTrackCount(topTrack0);
    bool grouped = (topAfter == topBefore - 1) && (nestedAfter == nestedBefore + 1);

    // Round-trip the nested arrangement through save/load (live untouched).
    QString tmpPath = QDir::tempPath() + "/smaragd_group.qxp";
    bool saved = SSaveProjectAction(tmpPath).apply(currentProject_).applied;
    bool nestedRoundTrips = false;
    int probeTop = -1;
    if (saved) {
        SProject *probe = new SProject();
        if (SLoadProjectAction(tmpPath).apply(probe).applied) {
            SStdMixer *pm = dynamic_cast<SStdMixer*>(probe->getRootComponent());
            if (pm && pm->getNTracks() >= 1) {
                probeTop = pm->getNTracks();
                SObject *pTop0 = &pm->childAt(0)->getSObject();
                nestedRoundTrips = (probeTop == topAfter)
                                   && (childTrackCount(pTop0) == nestedAfter);
            }
        }
        delete probe;
    }

    // Undo the grouping; the flat arrangement should return.
    SApplication::app().actionHistory()->undo();
    bool undone = (mixer->getNTracks() == topBefore)
                  && (childTrackCount(topTrack0) == nestedBefore);

    bool ok = grouped && nestedRoundTrips && undone;
    QString msg = ok
        ? QString("Group test OK: tree built, round-tripped, undone")
        : QString("Group test FAILED: grouped=%1 roundtrip=%2 undone=%3")
              .arg(grouped).arg(nestedRoundTrips).arg(undone);
    fprintf(stderr, "%s (top %d->%d->undo %d; probeTop=%d)\n",
            msg.toUtf8().constData(), topBefore, topAfter,
            mixer->getNTracks(), probeTop);
    fflush(stderr);
    statusBar()->showMessage(msg, 6000);
}

// Validate undoable track-remove: group track 1 under track 0 so the folder has
// a subtree, remove the folder, then undo and confirm the folder AND its nested
// child come back as the *same* objects (the pin preserves identity + subtree).
void SMainWindow::runUndoRemoveTest()
{
    if (!currentProject_) {
        statusBar()->showMessage("Open or create a project first", 3000);
        return;
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(currentProject_->getRootComponent());
    if (!mixer) {
        statusBar()->showMessage("Remove test FAILED: no mixer", 4000);
        return;
    }
    auto childTrackCount = [](SObject *c) {
        int n = 0;
        for (SLink *lk : c->childLinks())
            if (dynamic_cast<STrack*>(&lk->getSObject())) ++n;
        return n;
    };

    while (mixer->getNTracks() < 2) {
        SApplication::app().submitAction(new SAddTrackAction(-1));
    }
    SApplication::app().submitAction(new SReparentTrackAction(QList<int>{1}, QList<int>{0}));
    int topAfterGroup = mixer->getNTracks();
    STrack *folderTrack = dynamic_cast<STrack*>(&mixer->childAt(0)->getSObject());
    int childCount = folderTrack ? childTrackCount(folderTrack) : -1;

    SApplication::app().submitAction(new SRemoveTrackAction(0));
    int topAfterRemove = mixer->getNTracks();

    SApplication::app().actionHistory()->undo();
    int topAfterUndo = mixer->getNTracks();
    STrack *restored = mixer->getNTracks() > 0
        ? dynamic_cast<STrack*>(&mixer->childAt(0)->getSObject()) : nullptr;
    int restoredChildCount = restored ? childTrackCount(restored) : -1;
    bool identitySame = (restored == folderTrack);

    bool ok = (topAfterRemove == topAfterGroup-1)
              && (topAfterUndo == topAfterGroup)
              && (restoredChildCount == childCount)
              && identitySame;
    QString msg = ok
        ? QString("Undoable remove OK: folder+subtree restored (%1 child, same identity)").arg(childCount)
        : QString("Undoable remove FAILED: top %1->%2->undo %3; child %4->%5; identity=%6")
              .arg(topAfterGroup).arg(topAfterRemove).arg(topAfterUndo)
              .arg(childCount).arg(restoredChildCount).arg(identitySame);
    fprintf(stderr, "%s\n", msg.toUtf8().constData());
    fflush(stderr);
    statusBar()->showMessage(msg, 6000);
}

// MVP grain-playback trigger (proposal 06): set a time-stretch factor on the
// currently selected clip. Set it while playback is stopped (rebuild is not yet
// realtime-safe), then play.
void SMainWindow::runSetClipStretch()
{
    SLink *sel = SApplication::app().getCurrentSelectedSLink();
    SCut *cut = sel ? dynamic_cast<SCut*>( &sel->getSObject() ) : nullptr;
    if( !cut ) {
        statusBar()->showMessage( "Select a clip (SCut) first", 3000 );
        return;
    }
    bool ok = false;
    double s = QInputDialog::getDouble( this, "Clip Time-Stretch",
                                        "Stretch factor (>1 = longer/slower):",
                                        cut->getStretch(), 0.1, 10.0, 3, &ok );
    if( !ok ) return;
    cut->setStretch( s );
    statusBar()->showMessage( QString( "Clip stretch set to %1x" ).arg( s ), 4000 );
}

// MVP grain-playback trigger: set a pitch offset (cents) on the selected clip.
void SMainWindow::runSetClipPitch()
{
    SLink *sel = SApplication::app().getCurrentSelectedSLink();
    SCut *cut = sel ? dynamic_cast<SCut*>( &sel->getSObject() ) : nullptr;
    if( !cut ) {
        statusBar()->showMessage( "Select a clip (SCut) first", 3000 );
        return;
    }
    bool ok = false;
    double cents = QInputDialog::getDouble( this, "Clip Pitch Offset",
                                            "Pitch offset (cents, 1200 = +1 octave):",
                                            cut->getPitchCents(), -2400.0, 2400.0, 1, &ok );
    if( !ok ) return;
    cut->setPitchCents( cents );
    statusBar()->showMessage( QString( "Clip pitch set to %1 cents" ).arg( cents ), 4000 );
}

// Create a persistent nesting so the indented arranger is visible (the Group
// Track Test self-undoes). Nests track 1 under track 0; Ctrl+Z ungroups.
void SMainWindow::runGroupPersist()
{
    if (!currentProject_) {
        statusBar()->showMessage("Open or create a project first", 3000);
        return;
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(currentProject_->getRootComponent());
    if (!mixer) return;
    while (mixer->getNTracks() < 2) {
        SApplication::app().submitAction(new SAddTrackAction(-1));
    }
    SApplication::app().submitAction(
        new SReparentTrackAction(QList<int>{1}, QList<int>{0}));
    statusBar()->showMessage("Nested track 1 under track 0 (Ctrl+Z to ungroup)", 4000);
}

// Validation for exact-slot reorder: tag the first three tracks with distinct
// volumes (0,1,2 dB) as identity, move track 0 to slot 2 via SMoveTrackAction,
// confirm the new order, round-trip it, and confirm undo restores the exact
// original order. Self-contained.
void SMainWindow::runReorderTrackTest()
{
    if (!currentProject_) {
        statusBar()->showMessage("Open or create a project first", 3000);
        return;
    }
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(currentProject_->getRootComponent());
    if (!mixer) {
        statusBar()->showMessage("Reorder test FAILED: no mixer", 4000);
        return;
    }

    while (mixer->getNTracks() < 3) {
        SApplication::app().submitAction(new SAddTrackAction(-1));
    }

    auto trackAt = [mixer](int i) {
        return dynamic_cast<STrack*>(&mixer->getTrackAt(i)->getSObject());
    };
    for (int i = 0; i < 3; ++i) trackAt(i)->setVolume((double)i);

    // Order signature: the integer volume tag of each top-level track.
    auto orderString = [mixer, trackAt]() {
        QString s;
        for (int i = 0; i < mixer->getNTracks(); ++i)
            s += QString::number((int)trackAt(i)->getVolume());
        return s;
    };

    const QString before = orderString();          // "012..."
    const int n = mixer->getNTracks();

    // Move the first track to slot 2.
    SApplication::app().submitAction(new SMoveTrackAction(QList<int>{0}, 2));
    const QString moved = orderString();           // expect "120..."
    bool reordered = (moved.left(3) == "120");

    // Round-trip the reordered arrangement.
    QString tmpPath = QDir::tempPath() + "/smaragd_reorder.qxp";
    bool saved = SSaveProjectAction(tmpPath).apply(currentProject_).applied;
    bool orderRoundTrips = false;
    if (saved) {
        SProject *probe = new SProject();
        if (SLoadProjectAction(tmpPath).apply(probe).applied) {
            SStdMixer *pm = dynamic_cast<SStdMixer*>(probe->getRootComponent());
            if (pm && pm->getNTracks() >= 3) {
                QString ps;
                for (int i = 0; i < 3; ++i)
                    ps += QString::number(
                        (int)dynamic_cast<STrack*>(&pm->getTrackAt(i)->getSObject())->getVolume());
                orderRoundTrips = (ps == "120");
            }
        }
        delete probe;
    }

    // Undo restores the exact original order.
    SApplication::app().actionHistory()->undo();
    const QString undone = orderString();
    bool restored = (undone == before) && (mixer->getNTracks() == n);

    bool ok = reordered && orderRoundTrips && restored;
    QString msg = ok
        ? QString("Reorder test OK: %1 -> %2 -> undo %3")
              .arg(before.left(3)).arg(moved.left(3)).arg(undone.left(3))
        : QString("Reorder test FAILED: reordered=%1 roundtrip=%2 restored=%3 (%4->%5->%6)")
              .arg(reordered).arg(orderRoundTrips).arg(restored)
              .arg(before.left(3)).arg(moved.left(3)).arg(undone.left(3));
    fprintf(stderr, "%s\n", msg.toUtf8().constData());
    fflush(stderr);
    statusBar()->showMessage(msg, 6000);
}

// Draw a small square "lamp" icon with a single glyph, for the palette buttons.
static QIcon makePaletteIcon( const QString &glyph )
{
    const int sz = 22;

    // Normal state (unchecked)
    QPixmap pm_normal( sz, sz );
    pm_normal.fill( Qt::transparent );
    QPainter pr_normal( &pm_normal );
    pr_normal.setRenderHint( QPainter::Antialiasing, false );
    pr_normal.setPen( QColor( 80, 80, 80 ) );
    pr_normal.setBrush( QColor( 235, 235, 225 ) );
    pr_normal.drawRect( 2, 2, sz - 5, sz - 5 );
    pr_normal.setPen( QColor( 40, 40, 40 ) );
    pr_normal.setFont( QFont( "Helvetica Neue", 9, QFont::Bold ) );
    pr_normal.drawText( QRect( 2, 2, sz - 5, sz - 5 ), Qt::AlignCenter, glyph );
    pr_normal.end();

    // Checked state: darker background with highlight
    QPixmap pm_checked( sz, sz );
    pm_checked.fill( Qt::transparent );
    QPainter pr_checked( &pm_checked );
    pr_checked.setRenderHint( QPainter::Antialiasing, false );
    pr_checked.setPen( QColor( 40, 80, 160 ) );  // Blue border for checked state
    pr_checked.setBrush( QColor( 180, 200, 240 ) );  // Light blue background
    pr_checked.drawRect( 2, 2, sz - 5, sz - 5 );
    pr_checked.setPen( QColor( 20, 40, 100 ) );  // Dark blue text
    pr_checked.setFont( QFont( "Helvetica Neue", 9, QFont::Bold ) );
    pr_checked.drawText( QRect( 2, 2, sz - 5, sz - 5 ), Qt::AlignCenter, glyph );
    pr_checked.end();

    // Create icon with both states
    QIcon icon;
    icon.addPixmap( pm_normal, QIcon::Normal, QIcon::Off );
    icon.addPixmap( pm_checked, QIcon::Normal, QIcon::On );

    // Optional: Add active/pressed state for visual feedback
    icon.addPixmap( pm_checked, QIcon::Active, QIcon::On );

    return icon;
}

void SMainWindow::buildPaletteToolbar()
{
    // Use new grid toolbar for compact, Reaper-like layout
    qTBPalette_ = new SGridToolbar( "Palette", this );
    qTBPalette_->setObjectName( "toolbar_palette" );
    qTBPalette_->setColumns( 7 );  // 7 columns like Reaper
    qTBPalette_->setButtonSize( 24 );

    // Each toggle is a small, square, checkable button with a shortcut. Clicking
    // it (or pressing the shortcut) submits a *-toggle action against the current
    // project; the button's checked state is kept in sync from the project's
    // propertyChanged signal (see syncPaletteToProject / onProjectPropertyChanged),
    // so it tracks the setting however it changes (button, shortcut, or script).
    auto addToggle = [&]( const QString &glyph, const QString &name,
                          const QKeySequence &sc, const char *slot ) -> QAction* {
        QAction *a = new QAction( makePaletteIcon( glyph ), name, this );
        a->setCheckable( true );
        a->setShortcut( sc );
        a->setToolTip( QString( "%1 (%2)" ).arg( name, sc.toString() ) );
        a->setEnabled( false );   // no project yet; enabled by syncPaletteToProject
        QObject::connect( a, SIGNAL( triggered() ), this, slot );
        qTBPalette_->addGridAction( a );  // Use grid layout instead of linear
        return a;
    };

    actSnapToGrid_ = addToggle( "S", "Snap to grid", Qt::ALT | Qt::Key_S, SLOT( toggleSnapToGrid() ) );
    actGrid_       = addToggle( "G", "Grid",         Qt::Key_G, SLOT( toggleGrid() ) );
    actMetronome_  = addToggle( "M", "Metronome",    Qt::Key_M, SLOT( toggleMetronome() ) );
    actCycle_      = addToggle( "C", "Cycle",        Qt::Key_C, SLOT( toggleCycle() ) );

    addToolBar( Qt::TopToolBarArea, qTBPalette_ );

    // Track grouping toolbar. These act on the arranger's last-clicked track
    // (click a track lane, then Group/Ungroup).
    qTBTracks_ = new QToolBar( "Tracks" );
    qTBTracks_->setObjectName( "toolbar_tracks" );
    qTBTracks_->setIconSize( QSize( 22, 22 ) );
    QAction *aGroup = new QAction( makePaletteIcon( "[" ), "Group track", this );
    aGroup->setToolTip( "Group: wrap the clicked track in a new folder" );
    QObject::connect( aGroup, SIGNAL( triggered() ), this, SLOT( groupTrack() ) );
    qTBTracks_->addAction( aGroup );
    QAction *aUngroup = new QAction( makePaletteIcon( "]" ), "Ungroup track", this );
    aUngroup->setToolTip( "Ungroup: dissolve the clicked folder track" );
    QObject::connect( aUngroup, SIGNAL( triggered() ), this, SLOT( ungroupTrack() ) );
    qTBTracks_->addAction( aUngroup );
    addToolBar( Qt::TopToolBarArea, qTBTracks_ );

    // Reflect whatever project is current at startup (usually none).
    syncPaletteToProject( currentProject_ );
}

void SMainWindow::syncPaletteToProject( SProject *project )
{
    const bool en = ( project != NULL );
    actSnapToGrid_->setEnabled( en );
    actGrid_->setEnabled( en );
    actMetronome_->setEnabled( en );
    actCycle_->setEnabled( en );

    if( !project ) return;

    // setChecked does not emit triggered(), so this won't re-submit actions.
    actSnapToGrid_->setChecked( project->prop( SProjectProps::SnapToGrid, true ).toBool() );
    actGrid_->setChecked( project->prop( SProjectProps::GridVisible, true ).toBool() );
    actMetronome_->setChecked( project->prop( SProjectProps::Metronome, false ).toBool() );
    actCycle_->setChecked( project->prop( SProjectProps::Cycle, false ).toBool() );

    // The previous project (if any) was deleted by closeProject(), which
    // auto-removed its connections, so we only need to connect the new one.
    QObject::connect( project, SIGNAL( propertyChanged( QString, QVariant ) ),
                      this, SLOT( onProjectPropertyChanged( QString, QVariant ) ) );
}

void SMainWindow::onProjectPropertyChanged( const QString &key, const QVariant &value )
{
    if( key == SProjectProps::SnapToGrid )       actSnapToGrid_->setChecked( value.toBool() );
    else if( key == SProjectProps::GridVisible ) actGrid_->setChecked( value.toBool() );
    else if( key == SProjectProps::Metronome )   actMetronome_->setChecked( value.toBool() );
    else if( key == SProjectProps::Cycle )       actCycle_->setChecked( value.toBool() );

    // Cycle toggle or a change to the range markers updates the live loop region.
    if( key == SProjectProps::Cycle || key == SProjectProps::RangeValid
        || key == SProjectProps::RangeStart || key == SProjectProps::RangeEnd ) {
        syncCyclePlayback();
    }
}

// Push the project's cycle flag and time-range bounds to the speaker, which
// performs the seamless loop in its render callback. Cycling needs a valid
// range, so it is off whenever no range is set.
void SMainWindow::syncCyclePlayback()
{
    twSpeaker *speaker = SApplication::app().getSpeaker();
    if( !speaker ) return;

    if( !currentProject_ ) {
        speaker->setCycle( false, 0, 0 );
        return;
    }

    bool cycle = currentProject_->prop( SProjectProps::Cycle, false ).toBool();
    offset_t start = (offset_t) currentProject_->prop(
                         SProjectProps::RangeStart, (qulonglong) 0 ).toULongLong();
    offset_t end   = (offset_t) currentProject_->prop(
                         SProjectProps::RangeEnd,   (qulonglong) 0 ).toULongLong();
    bool haveRange = currentProject_->prop( SProjectProps::RangeValid, false ).toBool();

    speaker->setCycle( cycle && haveRange, start, end );
}

void SMainWindow::toggleSnapToGrid()
{
    SApplication::app().submitAction( new SSnapToGridAction( SToggleSettingAction::Toggle ) );
}

void SMainWindow::toggleGrid()
{
    SApplication::app().submitAction( new SGridAction( SToggleSettingAction::Toggle ) );
}

void SMainWindow::toggleMetronome()
{
    SApplication::app().submitAction( new SMetronomeAction( SToggleSettingAction::Toggle ) );
}

void SMainWindow::toggleCycle()
{
    SApplication::app().submitAction( new SCycleAction( SToggleSettingAction::Toggle ) );
}

void SMainWindow::groupTrack()
{
    SStdMixerView *v = dynamic_cast<SStdMixerView*>( projectRootWidget_ );
    if( v ) v->ctGroupTrack();
}

void SMainWindow::ungroupTrack()
{
    SStdMixerView *v = dynamic_cast<SStdMixerView*>( projectRootWidget_ );
    if( v ) v->ctUngroupTrack();
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

void SMainWindow::showOptionsDialog()
{
    SOptionsDialog dlg( this );
    dlg.exec();   // pages write to SSettings on OK/Apply; live UI reacts to changed()
}

void SMainWindow::measureAudioLatenciesIfNeeded()
{
    SSettings &settings = SSettings::instance();
    QString outDeviceId = settings.audioDeviceId();
    QString inDeviceId = settings.audioInputDeviceId();

    // Check if both latencies are already cached
    bool outKnown = settings.audioOutputLatencyFrames( outDeviceId ) > 0;
    bool inKnown = settings.audioInputLatencyFrames( inDeviceId ) > 0;

    if( outKnown && inKnown ) {
        return;  // Both latencies cached, nothing to do
    }

    // Show modal dialog while measuring
    QDialog progressDlg( this );
    progressDlg.setWindowTitle( "Initializing Audio" );
    progressDlg.setModal( true );
    progressDlg.setWindowFlags( progressDlg.windowFlags() & ~Qt::WindowContextHelpButtonHint );

    QVBoxLayout *layout = new QVBoxLayout( &progressDlg );
    QLabel *label = new QLabel( "Checking audio devices..." );
    layout->addWidget( label );

    progressDlg.setMinimumWidth( 300 );
    progressDlg.setMinimumHeight( 100 );
    progressDlg.show();
    QApplication::processEvents();

    // Measure output latency if not cached
    if( !outKnown ) {
        twSpeaker *spk = SApplication::app().getSpeaker();
        if( spk ) {
            audio::AudioBackend *backend = spk->getBackend();
            if( backend ) {
                uint32_t latency = backend->getLatencyFrames();
                if( latency > 0 ) {
                    settings.setAudioOutputLatencyFrames( outDeviceId, latency );
                }
            }
        }
    }

    // Measure input latency if not cached
    if( !inKnown ) {
        std::unique_ptr<audio::AudioInput> input = audio::createAudioInput();
        if( input ) {
            // Try to open with a 500ms timeout
            if( input->openDevice( inDeviceId.toStdString(), 48000 ) == 0 ) {
                uint32_t latency = input->getLatencyFrames();
                if( latency > 0 ) {
                    settings.setAudioInputLatencyFrames( inDeviceId, latency );
                }
                input->closeDevice();
            }
        }
    }

    progressDlg.close();
}

SMainWindow::~SMainWindow()
{
}

