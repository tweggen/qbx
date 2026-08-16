
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
#include "app/objects/midi/smidiclipactions.h"
#include "app/model/splacements.h"
#include "app/model/sobjectpath.h"
#include "app/shell/ssettings.h"
#include "app/servicesui/srecordingprogress.h"
#include "app/servicesui/slogview.h"
#include "app/servicesui/soptions.h"
#include "app/objects/cut/scut.h"
#include "app/objects/wave/splainwave.h"
#include "app/model/slink.h"
#include "app/objects/track/strack.h"
#include "app/objects/cut/splacerecordingaction.h"
#include "app/objects/cut/ssetpitchaction.h"
#include "app/actions/sactionhistory.h"
#include <QUndoStack>
#include <QPair>
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
#include "app/eventui/seventeditordock.h"
#include "app/eventui/seventtimeaxis.h"
#include "app/eventui/spianorollview.h"
#include "app/eventui/svirtualkeyboarddock.h"
#include "app/timeline/slevelmeter.h"
#include "app/timeline/ssmvmixercontrol.h"
#include "app/timeline/sclippropertiespanel.h"
#include "app/timeline/strackdetailpanel.h"
#include "app/objects/track/strack.h"
#include "app/servicesui/soptionsdialog.h"

#include "tw/playback/twspeaker.h"
#include "tw/devices/audio_input.h"

#include "app/objects/mixer/saddtrackaction.h"
#include "app/objects/mixer/sreparenttrackaction.h"
#include "app/objects/mixer/smovetrackaction.h"
#include "app/objects/mixer/sremovetrackaction.h"
#include "app/objects/cut/saddsampleaction.h"
#include "app/objects/track/ssettrackvolumeaction.h"
#include "app/persistence/ssaveprojectaction.h"
#include "app/persistence/sloadprojectaction.h"
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

#include "pix/recon.xpm"
#include "pix/recoff.xpm"
#include "pix/playoff.xpm"
#include "pix/playon.xpm"
#include "pix/stopoff.xpm"
#include "tw/core/twlog.h"

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
    // Drop the detail panel's view of the (about to die) project's tracks.
    detachTrackDetail();
    detachClipProperties();
    detachEventEditor();
}

void SMainWindow::attachTrackDetail()
{
    if( !trackDetailPanel_ ) return;

    // One mixer at a time: drop whatever we were following first.
    QObject::disconnect( trackDetailConn_ );

    SStdMixer *mixer = currentProject_
        ? qobject_cast<SStdMixer*>( currentProject_->getRootComponent() )
        : nullptr;
    if( !mixer ) {
        trackDetailPanel_->setTrack( nullptr );
        return;
    }

    trackDetailConn_ = connect( mixer, &SStdMixer::selectedTrackChanged,
                                trackDetailPanel_, &STrackDetailPanel::setTrack );
    trackDetailPanel_->setTrack( mixer->getSelectedTrack() );
}

void SMainWindow::detachTrackDetail()
{
    QObject::disconnect( trackDetailConn_ );
    trackDetailConn_ = QMetaObject::Connection();
    // Release the STrack pointer (and its plugin strip) BEFORE the project is
    // deleted — the panel holds a raw pointer into the project's object graph.
    if( trackDetailPanel_ ) trackDetailPanel_->setTrack( nullptr );
}

void SMainWindow::attachClipProperties()
{
    if( !clipPropsPanel_ ) return;

    QObject::disconnect( clipPropsConn_ );

    if( !currentProject_ ) {
        clipPropsPanel_->refresh();     // resolves to "no clip selected"
        return;
    }

    // The panel follows the SELECTION, but selection changes are themselves
    // actions, so they already end in notifyArrangementChanged() — the same
    // signal the arranger repaints on. Piggy-backing on it means no new signal
    // and no polling, and it picks up edge drags, undo and .qxa scripts too.
    clipPropsConn_ = connect( currentProject_, &SProject::arrangementChanged,
                              clipPropsPanel_,
                              &SClipPropertiesPanel::refresh );
    clipPropsPanel_->refresh();
}

void SMainWindow::detachClipProperties()
{
    QObject::disconnect( clipPropsConn_ );
    clipPropsConn_ = QMetaObject::Connection();
    // Drop every SLink/SCut the panel resolved BEFORE the project dies. The
    // panel caches no pointers between refreshes, so an empty refresh is all
    // that is needed — but it must happen while the graph is still alive.
    if( clipPropsPanel_ ) clipPropsPanel_->refresh();
}

// Same lifecycle as the clip properties dock, and for the same reason: the
// event editor follows the SELECTION, and selection changes are actions, so
// they already end in notifyArrangementChanged(). No new signal, no polling,
// and undo / .qxa scripts are picked up for free (timeline invariant 8).
void SMainWindow::attachEventEditor()
{
    if( !eventEditor_ ) return;

    QObject::disconnect( eventEditorConn_ );

    if( !currentProject_ ) {
        eventEditor_->refresh();      // resolves to "no event clip selected"
        return;
    }
    if( SEventTimeAxis *axis = eventEditor_->timeAxis() )
        axis->setSampleRate( currentProject_->getSRate() );

    eventEditorConn_ = connect( currentProject_, &SProject::arrangementChanged,
                                eventEditor_, &SEventEditorDock::refresh );
    eventEditor_->refresh();
}

void SMainWindow::detachEventEditor()
{
    QObject::disconnect( eventEditorConn_ );
    eventEditorConn_ = QMetaObject::Connection();
    // Drop every SLink the dock resolved BEFORE the project dies. It caches no
    // pointers between refreshes, so an empty refresh is the whole cleanup -
    // but it has to happen while the graph is still alive.
    if( eventEditor_ ) eventEditor_->refresh();
}

// The arranger's zoom/scroll -> the editor's axis. Wired here because the shell
// is the ONLY module that sees both app/timeline and app/eventui: the editor
// must not depend on the 4000-line arranger just to share a time axis.
void SMainWindow::linkEventEditorAxis()
{
    if( !eventEditor_ ) return;
    SEventTimeAxis *axis = eventEditor_->timeAxis();
    SStdMixerView *v = dynamic_cast<SStdMixerView *>( projectRootWidget_ );
    if( !axis || !v ) return;

    SMVActualView *content = v->contentView();
    if( !content ) return;

    QObject::disconnect( axisZoomConn_ );
    QObject::disconnect( axisScrollConn_ );

    // Seed once, then follow. Both signals matter: a zoom without a scroll and
    // a scroll without a zoom each move the mapping on their own.
    axis->setSecondWidth( content->getSecondWidth() );
    axis->setLeftPixels( (int) content->getUpperLeftX() );

    axisZoomConn_ = connect( content, &SMVActualView::secondWidthChanged, axis,
             [axis, content]( double w ) {
                 if( !axis->linked() ) return;
                 axis->setSecondWidth( w );
                 axis->setLeftPixels( (int) content->getUpperLeftX() );
             } );
    axisScrollConn_ = connect( content, &SMVActualView::leftOffsetChanged, axis,
             [axis, content]( offset_t ) {
                 if( !axis->linked() ) return;
                 axis->setLeftPixels( (int) content->getUpperLeftX() );
             } );
}

void SMainWindow::showClipProperties()
{
    if( !qDockClipProps_ ) return;

    // Round-trip the binding: if it is already up AND focused, F2 closes it.
    // "Focused" rather than merely visible, so F2 from the arranger always
    // brings the panel forward instead of hiding a panel the user is watching.
    if( qDockClipProps_->isVisible() && clipPropsPanel_
        && clipPropsPanel_->isAncestorOf( QApplication::focusWidget() ) ) {
        qDockClipProps_->hide();
        return;
    }

    qDockClipProps_->show();
    qDockClipProps_->raise();       // pulls it out of a tab group
    if( clipPropsPanel_ ) {
        clipPropsPanel_->refresh();
        clipPropsPanel_->focusFirstField();
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
    linkEventEditorAxis();

    setCentralWidget( projectRootWidget_ );
    projectRootWidget_->show();
    SApplication::app().setCurrentProject( currentProject_ );
    attachTrackDetail();
    attachClipProperties();
    attachEventEditor();

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
    linkEventEditorAxis();

    setCentralWidget( projectRootWidget_ );
    projectRootWidget_->show();
    SApplication::app().setCurrentProject( currentProject_ );
    attachTrackDetail();
    attachClipProperties();
    attachEventEditor();

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
    linkEventEditorAxis();
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

    // Drop the undo history BEFORE the project dies. Commands on the stack hold
    // raw pointers into the project's object graph and some hold a refcount pin
    // (SRemoveTrackAction::heldTrack_); the history is owned by SApplication, so
    // it outlives the project and its destructors would otherwise call
    // removeRef() on freed objects. Clearing here releases every pin while the
    // objects are still alive, which also lets the project's refcount cascade
    // reach them instead of hard-deleting them out from under live SLinks.
    if( SActionHistory *history = SApplication::app().actionHistory() ) {
        history->undoStack()->clear();
    }

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
        // NO graph seek here. Play start used to walk the whole model tree
        // seeking every component to the locator; that cascade takes only each
        // component's mutex(), never its cursorMutex_, so it could land between
        // an in-flight freeze's seekTo(startPos) and its renderFrames() and fill
        // a whole 65536-frame page with audio from the seek target — cached
        // under the ORIGINAL startPos and stamped valid. Position is carried by
        // the page (freezePage_nolock seeks per page, under cursorMutex_) and by
        // the engine's own locator; the offline render dropped its cursor
        // cascade for the same reason and is the exactness gate.
        // Arm cycle (loop) playback from the current project state before output
        // starts, so the loop region is honoured from the first buffer.
        syncCyclePlayback();

        // THE RUN BARRIER (proposal 37 D4 / 4.4), on the main thread and
        // immediately before startOutput() - which is what performs the
        // engine's pre-readahead seekTo(locator) + startReadahead(), so the
        // barrier is ordered ahead of the readahead's first demand. The GUI's
        // Play button reaches the speaker here rather than through
        // SApplication::setPlaybackRunning(), so it needs its own call: a
        // barrier issued on only ONE of the two play paths would make
        // determinism depend on which button was pressed.
        SApplication::app().beginRun( SApplication::app().getGlobalLocatorPos() );

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

// Collect armed tracks depth-first WITH their root-relative paths — nested
// (folder) tracks record too. The order is the contract between recording
// start (armedTrackIds) and completion (createdFiles are matched
// positionally), so both sides call this.
static void collectArmedTracks( SObject *container, const QList<int> &path,
                                QList<QPair<STrack *, QList<int>>> &out )
{
    if( !container ) return;
    for( int i = 0; i < container->childCount(); ++i ) {
        SLink *lk = container->childAt( i );
        if( !lk ) continue;
        STrack *track = dynamic_cast<STrack *>( &lk->getSObject() );
        if( !track ) continue;
        QList<int> childPath = path;
        childPath.append( i );
        if( track->isArmedForRecording() ) {
            out.append( qMakePair( track, childPath ) );
        }
        collectArmedTracks( track, childPath, out );
    }
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
        actRecord_->setIcon( QIcon( QPixmap( (const char **)recoff_xpm ) ) );
    } else {
        // Check if any tracks are armed (recursively — tracks nested in
        // folder tracks record too, proposal 17 phase 2)
        QList<QPair<STrack *, QList<int>>> armed;
        collectArmedTracks( currentProject_->getRootComponent(), QList<int>(), armed );

        if( armed.isEmpty() ) {
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

        // Collect armed track IDs and per-track channel selections, in the
        // SAME recursive order onRecordingCompleted will use — created files
        // are matched to tracks positionally.
        for( const auto &pr : armed ) {
            params.armedTrackIds.push_back( pr.first->getSName().toStdString() );
            params.trackChannels.push_back( pr.first->getRecordingChannels() );
        }

        // Remember where the playhead is now: the cut goes here, and the playhead
        // advances from here during the capture.
        recordingStartPos_ = SApplication::app().getGlobalLocatorPos();

        // Note: latency sync offset will be calculated in onRecordingCompleted()
        // after the input latency is known from the recording session.
        recordingLatencySyncOffset_ = 0;

        SApplication::app().startRecording( params );
        actRecord_->setIcon( QIcon( QPixmap( (const char **)recon_xpm ) ) );

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
    auto speaker = SApplication::app().getSpeaker();
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

    // Place the recordings through the action system (proposal 17 phase 2):
    // one place-recording per armed track, all inside ONE undo macro. The
    // action plans the file against the track's existing columns — new take
    // per covered column (auto-activated), plain cuts for the gaps — so
    // recording over material stacks takes instead of layering clips, and
    // Ctrl-Z removes the whole recording pass.
    QList<QPair<STrack *, QList<int>>> armed;
    collectArmedTracks( currentProject_->getRootComponent(), QList<int>(), armed );

    QUndoStack *undoStack = SApplication::app().actionHistory()->undoStack();
    const bool macro = !armed.isEmpty() && undoStack;
    if( macro ) undoStack->beginMacro( QStringLiteral( "Recording" ) );
    int fileIndex = 0;
    for( const auto &pr : armed ) {
        STrack *track = pr.first;
        if( fileIndex < (int)createdFiles.size() ) {
            QString recordedFile = QString::fromStdString( createdFiles[fileIndex] );
            if( QFileInfo( recordedFile ).exists() ) {
                SApplication::app().submitAction( new SPlaceRecordingAction(
                    pr.second, recordedFile, recordingStartTime ) );
            }
        }
        // Auto-disarm stays a direct UI-state mutation (not undoable).
        track->setArmedForRecording( false );
        fileIndex++;
    }
    if( macro ) undoStack->endMacro();

    // Return the playhead to where recording began, lining it up with the cut we
    // just placed (it had advanced to the end during capture).
    SApplication::app().setGlobalLocatorPos( recordingStartTime );

    // Refresh the UI to display the newly placed clip
    if( projectRootWidget_ ) {
        projectRootWidget_->update();
    }

    actRecord_->setIcon( QIcon( QPixmap( (const char **)recoff_xpm ) ) );
}

SMainWindow::SMainWindow()
    : QMainWindow(),
      currentProject_( 0 ),
      projectRootWidget_( NULL )
{
    TW_LOGD( "ui.shell", "*** SMainWindow built on %s at %s ***", __DATE__, __TIME__ );

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
        QIcon( QPixmap( (const char **)recoff_xpm )),
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

    // Tempo (BPM) box next to the transport controls. Disabled until a project
    // is current; kept in sync both ways by syncPaletteToProject.
    tempoSpin_ = new QDoubleSpinBox( this );
    tempoSpin_->setDecimals( 3 );              // 3 digits after the decimal
    tempoSpin_->setRange( 1.0, 9999.999 );     // up to 4 digits before the decimal
    tempoSpin_->setKeyboardTracking( false );  // commit on Enter/focus-out
    tempoSpin_->setToolTip( "Tempo (BPM)" );
    tempoSpin_->setFocusPolicy(Qt::ClickFocus);
    tempoSpin_->setEnabled( false );           // enabled by syncPaletteToProject
    qTBTransport_->addWidget( tempoSpin_ );
    QObject::connect( tempoSpin_, SIGNAL( valueChanged(double) ),
                      this, SLOT( onTempoSpinChanged(double) ) );
    // Return in the tempo box commits and then hands the keyboard back, so the
    // next transport/arranger key does not disappear into the spin box. The
    // "back" target is whoever had focus when the box took it — remembered here
    // because a FocusIn event does not carry the widget it came from.
    tempoSpin_->installEventFilter( this );
    QObject::connect( qApp, &QApplication::focusChanged, this,
                      [this]( QWidget *old, QWidget *now ) {
                          if( now == tempoSpin_ && old != tempoSpin_ )
                              tempoPrevFocus_ = old;
                      } );

    // Master level meter (proposal 34), right of the tempo box. It reads the
    // frozen pages of the component the engine plays — the same probe mechanism
    // the track heads use, so master and tracks are mutually consistent.
    qMasterMeter_ = new SLevelMeter( this );
    qMasterMeter_->setOrientation( Qt::Horizontal );
    qMasterMeter_->setMinimumWidth( 96 );
    qMasterMeter_->setMaximumWidth( 96 );
    qMasterMeter_->setToolTip( "Master level — click to clear the clip indicator" );
    qTBTransport_->addWidget( qMasterMeter_ );
    QObject::connect( &SApplication::app(), &SApplication::meterTick, this,
                      [this]( offset_t pos, qint64 nowMs, bool live ) {
                          if( !qMasterMeter_ || !qMasterMeter_->isVisible() ) return;
                          twLevelSample s;
                          if( live && SApplication::app().masterLevel( pos, s ) )
                              qMasterMeter_->pushLevel( s, nowMs );
                          else
                              qMasterMeter_->pushIdle( nowMs );
                      } );
    QObject::connect( &SApplication::app(), &SApplication::meterReset,
                      qMasterMeter_, &SLevelMeter::resetMeter );

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
    actSaveAs_ = qFileMenu_->addAction( "Save &as...", Qt::CTRL | Qt::SHIFT | Qt::Key_S, this, SLOT( fileSaveAs() ) );
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
    // The "Set Clip Stretch/Pitch..." prompts that used to live here are gone
    // (proposal 31): the clip properties panel is the numeric-entry surface,
    // and it edits the whole selection through undoable actions. The stretch
    // prompt in particular wrote cut->setStretch() directly — the last
    // per-clip property mutation in the app that bypassed SAction.
    menuBar()->addMenu( qTestMenu_ );

    buildStatusBar();

    // Create the persistent extern file list dock. It outlives any single project;
    // its content is managed by setProject() called from createDocksToolbars.
    qDockExternFileList_ = new QDockWidget( tr( "Extern file list" ), this );
    qDockExternFileList_->setObjectName( "dock_extern_file_list" );
    externFileList_ = new SExternFileList( qDockExternFileList_, nullptr );
    qDockExternFileList_->setWidget( externFileList_ );
    addDockWidget( Qt::LeftDockWidgetArea, qDockExternFileList_ );

    // The track detail panel, docked directly BELOW the extern file list in the
    // same left area. It used to be a child of the arranger's track-control
    // column; as a dock it is persistent (project-independent) and the user can
    // move, float or close it. attachTrackDetail() wires it to a project's mixer.
    qDockTrackDetail_ = new QDockWidget( tr( "Track Detail" ), this );
    qDockTrackDetail_->setObjectName( "dock_track_detail" );
    trackDetailPanel_ = new STrackDetailPanel( qDockTrackDetail_ );
    qDockTrackDetail_->setWidget( trackDetailPanel_ );
    addDockWidget( Qt::LeftDockWidgetArea, qDockTrackDetail_ );
    splitDockWidget( qDockExternFileList_, qDockTrackDetail_, Qt::Vertical );

    // The log dock (proposal 24). Hidden on a first run; from then on
    // restoreState() honours whatever the user left it as, keyed by objectName.
    // visibilityChanged drives the model's drain timer, so a closed log costs
    // nothing at all.
    qDockLog_ = new QDockWidget( tr( "Log" ), this );
    qDockLog_->setObjectName( "dock_log" );
    logView_ = new SLogView( qDockLog_ );
    qDockLog_->setWidget( logView_ );
    addDockWidget( Qt::BottomDockWidgetArea, qDockLog_ );
    qDockLog_->hide();
    connect( qDockLog_, &QDockWidget::visibilityChanged,
             logView_, &SLogView::setLive );

    // The clip properties dock (proposal 31) — ONE window for the whole app.
    // Like the log dock it is hidden on a first run and its objectName is what
    // makes restoreState() bring back whatever the user left it as (docked
    // area, floating, floating geometry, visibility), so it needs no settings
    // key. Created HERE, in the ctor, because restoreWindowLayout() runs later
    // and can only restore docks that already exist.
    qDockClipProps_ = new QDockWidget( tr( "Clip Properties" ), this );
    qDockClipProps_->setObjectName( "dock_clip_properties" );
    clipPropsPanel_ = new SClipPropertiesPanel( qDockClipProps_ );
    qDockClipProps_->setWidget( clipPropsPanel_ );
    addDockWidget( Qt::RightDockWidgetArea, qDockClipProps_ );
    qDockClipProps_->hide();

    // The event editor dock (proposal 37 P4) — the fifth dock, BOTTOM, tabified
    // with the Log so the two share the strip under the arranger. Created here
    // in the ctor for the same reason every other dock is (shell CONTRACT
    // inv. 4: restoreWindowLayout() runs later and can only restore docks that
    // already exist), and hidden on a first run.
    qDockEventEditor_ = new QDockWidget( tr( "Event Editor" ), this );
    qDockEventEditor_->setObjectName( "dock_event_editor" );
    eventEditor_ = new SEventEditorDock( qDockEventEditor_ );
    qDockEventEditor_->setWidget( eventEditor_ );
    addDockWidget( Qt::BottomDockWidgetArea, qDockEventEditor_ );
    tabifyDockWidget( qDockLog_, qDockEventEditor_ );
    qDockEventEditor_->hide();

    // The virtual keyboard (proposal 37 6.3). Bottom as well, beside the
    // editor. It inserts notes at the locator through `add-note` and is the
    // headless note source behind the `virtual-key` verb.
    qDockVirtualKeys_ = new QDockWidget( tr( "Virtual Keyboard" ), this );
    qDockVirtualKeys_->setObjectName( "dock_virtual_keyboard" );
    virtualKeys_ = new SVirtualKeyboardDock( qDockVirtualKeys_ );
    qDockVirtualKeys_->setWidget( virtualKeys_ );
    addDockWidget( Qt::BottomDockWidgetArea, qDockVirtualKeys_ );
    tabifyDockWidget( qDockEventEditor_, qDockVirtualKeys_ );
    qDockVirtualKeys_->hide();

    // View menu — built here rather than in the menu block above because it
    // needs the docks to exist for their toggleViewAction()s.
    QMenu *viewMenu = new QMenu( tr( "&View" ), this );
    QAction *actLog = qDockLog_->toggleViewAction();
    actLog->setText( tr( "&Log" ) );
    actLog->setShortcut( Qt::CTRL | Qt::SHIFT | Qt::Key_L );
    viewMenu->addAction( actLog );
    QAction *actFiles = qDockExternFileList_->toggleViewAction();
    actFiles->setText( tr( "&Extern file list" ) );
    viewMenu->addAction( actFiles );
    QAction *actDetail = qDockTrackDetail_->toggleViewAction();
    actDetail->setText( tr( "&Track detail" ) );
    viewMenu->addAction( actDetail );
    // The toggle carries NO shortcut: F2 is a separate action below, because it
    // must show+raise+focus rather than blind-toggle a dock that may be buried
    // in a tab group.
    QAction *actProps = qDockClipProps_->toggleViewAction();
    actProps->setText( tr( "Clip &properties" ) );
    viewMenu->addAction( actProps );
    QAction *actEvents = qDockEventEditor_->toggleViewAction();
    actEvents->setText( tr( "&Event editor" ) );
    actEvents->setShortcut( Qt::CTRL | Qt::SHIFT | Qt::Key_E );
    viewMenu->addAction( actEvents );
    QAction *actKeys = qDockVirtualKeys_->toggleViewAction();
    actKeys->setText( tr( "Virtual &keyboard" ) );
    viewMenu->addAction( actKeys );
    menuBar()->insertMenu( qAudioMenu_->menuAction(), viewMenu );

    // F2 (default) opens the clip properties panel. There is no keybinding UI,
    // so the sequence is read once from SSettings — making it a DEFAULT rather
    // than a constant. An unparseable string yields a null QKeySequence, which
    // simply leaves the action unbound (menu item still works).
    actClipProps_ = new QAction( tr( "Clip properties" ), this );
    actClipProps_->setShortcut( QKeySequence::fromString(
        SSettings::instance()
            .value( SOpt::ShortcutClipProperties,
                    SOpt::def( SOpt::ShortcutClipProperties ) ).toString() ) );
    connect( actClipProps_, &QAction::triggered,
             this, &SMainWindow::showClipProperties );
    addAction( actClipProps_ );   // window-wide, no menu home (cf. actGotoStart_)

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
void SMainWindow::setLogDockVisible( bool visible )
{
    if( !qDockLog_ ) return;
    qDockLog_->setVisible( visible );
    // setVisible() emits visibilityChanged, which drives the drain timer — but
    // only when the window itself is shown. In a headless test the dock is
    // never "visible" to Qt, so tell the view directly.
    if( logView_ ) logView_->setLive( visible );
}

int SMainWindow::logViewBacklog() const
{
    return logView_ ? logView_->backlog() : 0;
}

int SMainWindow::logViewRows() const
{
    return logView_ ? logView_->displayedRows() : 0;
}

qint64 SMainWindow::logViewWorstDrainMs() const
{
    return logView_ ? logView_->worstDrainMs() : 0;
}

void SMainWindow::logViewResetDrainStats()
{
    if( logView_ ) logView_->resetDrainStats();
}

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

    auto spk = SApplication::app().getSpeaker();
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
    TW_LOGD( "ui.shell", "runTestSequence() CALLED" );

    // Open file dialog to pick a WAV file.
    TW_LOGD( "ui.shell", "  Opening file dialog..." );
    QString lastDir = SSettings::instance().lastDir("sample", "");
    QFileDialog dialog(this, "Select a WAV file for the test sequence", lastDir, "WAV Files (*.wav);;All Files (*)");
    dialog.setOptions(QFileDialog::DontUseNativeDialog);
    QString filePath;
    if (dialog.exec() == QDialog::Accepted) {
        filePath = dialog.selectedFiles().first();
    }
    TW_LOGD( "ui.shell", "  File dialog closed. filePath=%s", filePath.toStdString().c_str() );

    if (filePath.isEmpty()) {
        TW_LOGD( "ui.shell", "  User cancelled. Returning." );
        return;
    }

    // Save the directory.
    QFileInfo fileInfo(filePath);
    SSettings::instance().setLastDir("sample", fileInfo.absolutePath());

    // Create a new project.
    TW_LOGD( "ui.shell", "  Creating new project..." );
    fileNew();

    if (!currentProject_) {
        TW_LOGE( "ui.shell", "  ERROR: Failed to create new project!" );
        QMessageBox::critical(this, "Error", "Failed to create new project");
        return;
    }

    TW_LOGD( "ui.shell", "  Submitting actions..." );
    // Submit the test sequence actions.
    // 1. Add a track at index 0.
    SApplication::app().submitAction(new SAddTrackAction(0));
    TW_LOGD( "ui.shell", "    Add track action submitted" );

    // 2. Add the sample to track 0 at time 0.
    SApplication::app().submitAction(new SAddSampleAction(QList<int>{0}, filePath, 0));
    TW_LOGD( "ui.shell", "    Add sample action submitted" );

    // 3. Start playback.
    SApplication::app().submitAction(new STogglePlaybackAction(true));
    TW_LOGD( "ui.shell", "    Toggle playback action submitted" );

    statusBar()->showMessage("Test sequence started", 2000);
    TW_LOGD( "ui.shell", "  Test sequence complete!" );
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
        SApplication::app().submitAction(new SSetTrackVolumeAction(QList<int>{0}, db));
    }

    int after = stack ? stack->count() : -1;
    TW_LOGD( "ui.shell", "Volume burst: %d actions submitted; undo stack %d -> %d "
                    "(expect +1 if merge worked)", steps, before, after );
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
    // The duration is the ARRANGEMENT's now, so an empty project really is 0 —
    // and half of nothing is not a range anything can render.
    if (duration <= 0.0) {
        statusBar()->showMessage("Nothing in the arrangement to select", 3000);
        return;
    }
    currentProject_->setTimeSelection(0.0, duration / 2.0);
    statusBar()->showMessage(
        QString("Time selection set: 0.0 - %1 seconds").arg(duration / 2.0, 0, 'f', 2), 3000);
}

namespace {

// The diagnostic round trips below save the LIVE project to a scratch file, and
// SSaveProjectAction rightly re-anchors it there — external file references are
// stored relative to the project file, so the copy must be written against the
// path it is written to. That anchor is a lie once the probe is over (the
// user's project does not live in %TEMP%), so restore it and keep "the live
// project is never disturbed" true. The saved copy is unaffected.
class ScopedProjectAnchor {
public:
    explicit ScopedProjectAnchor( SProject *project )
        : project_( project ),
          saved_( project ? project->projectFilePath() : QString() ) {}
    ~ScopedProjectAnchor() {
        if( project_ ) project_->setProjectFilePath( saved_ );
    }
    ScopedProjectAnchor( const ScopedProjectAnchor & ) = delete;
    ScopedProjectAnchor &operator=( const ScopedProjectAnchor & ) = delete;
private:
    SProject *project_;
    QString   saved_;
};

} // namespace

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
    ScopedProjectAnchor anchor(currentProject_);
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
    TW_LOGD( "ui.shell", "%s  [%s]", msg.toUtf8().constData(), tmpPath.toUtf8().constData() );
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
    ScopedProjectAnchor anchor(currentProject_);
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
    TW_LOGD( "ui.shell", "%s (top %d->%d->undo %d; probeTop=%d)",
            msg.toUtf8().constData(), topBefore, topAfter,
            mixer->getNTracks(), probeTop );
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

    SApplication::app().submitAction(new SRemoveTrackAction(QList<int>{0}));
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
    TW_LOGD( "ui.shell", "%s", msg.toUtf8().constData() );
    statusBar()->showMessage(msg, 6000);
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
    ScopedProjectAnchor anchor(currentProject_);
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
    TW_LOGD( "ui.shell", "%s", msg.toUtf8().constData() );
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
    // "Save as..." and the tempo box only make sense with a project.
    if( actSaveAs_ ) actSaveAs_->setEnabled( en );
    if( tempoSpin_ ) tempoSpin_->setEnabled( en );

    if( !project ) return;

    // setChecked does not emit triggered(), so this won't re-submit actions.
    actSnapToGrid_->setChecked( project->prop( SProjectProps::SnapToGrid, true ).toBool() );
    actGrid_->setChecked( project->prop( SProjectProps::GridVisible, true ).toBool() );
    actMetronome_->setChecked( project->prop( SProjectProps::Metronome, false ).toBool() );
    actCycle_->setChecked( project->prop( SProjectProps::Cycle, false ).toBool() );

    // Seed the tempo box and keep it in sync with the project. blockSignals
    // avoids the seed re-submitting to the project. The prior project (if any)
    // was deleted by closeProject(), auto-removing its bpmTempoChanged link.
    if( tempoSpin_ ) {
        tempoSpin_->blockSignals( true );
        tempoSpin_->setValue( project->getBPMTempo() );
        tempoSpin_->blockSignals( false );
        QObject::connect( project, SIGNAL( bpmTempoChanged(double) ),
                          tempoSpin_, SLOT( setValue(double) ) );
    }

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
    auto speaker = SApplication::app().getSpeaker();
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

bool SMainWindow::eventFilter( QObject *watched, QEvent *event )
{
    if( watched == tempoSpin_ && event->type() == QEvent::KeyPress ) {
        QKeyEvent *ke = static_cast<QKeyEvent*>( event );
        if( ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter ) {
            // Let the spin box interpret and commit the typed text first (it is
            // keyboardTracking=false, so the value lands on this very key), then
            // give the focus back. Deferred to the event loop for exactly that
            // ordering — clearing focus inside the filter would commit through
            // the focus-out path instead and swallow the keypress semantics.
            QTimer::singleShot( 0, this, [this]() {
                if( !tempoSpin_ ) return;
                tempoSpin_->clearFocus();
                QWidget *back = tempoPrevFocus_;
                if( !back || !back->isVisible() ) back = projectRootWidget_;
                if( back ) back->setFocus( Qt::OtherFocusReason );
            } );
        }
    }
    return QMainWindow::eventFilter( watched, event );
}

// Headless test mode never opens the project through the window —
// SActionRunner sets it straight on SApplication — so the arranger does not
// exist yet. Build it the same way openProject() does; later test entry points
// in the same script then share this one view (and its zoom/scroll).
SStdMixerView *SMainWindow::ensureArranger_()
{
    SStdMixerView *v = dynamic_cast<SStdMixerView*>( projectRootWidget_ );
    if( v ) return v;
    SProject *proj = SApplication::app().getCurrentProject();
    if( !proj || !proj->getRootComponent() ) return NULL;
    projectRootWidget_ = proj->getRootComponent()->getDetailEditWidget( this );
    setCentralWidget( projectRootWidget_ );
    linkEventEditorAxis();
    return dynamic_cast<SStdMixerView*>( projectRootWidget_ );
}

bool SMainWindow::dragClipEdge( int rowIdx, int clipIdx, int grabWhere,
                                offset_t dropTime, bool upperHalf,
                                Qt::KeyboardModifiers mods )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return false;
    return v->dragClipEdge( rowIdx, clipIdx, grabWhere, dropTime, upperHalf, mods );
}

bool SMainWindow::groupTrackGesture( const QString &trackPath, bool ungroup )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return false;
    SProject *proj = SApplication::app().getCurrentProject();
    SObject *root = splacements::rootContainer( proj );
    SObject *lane = splacements::laneAt( root, strackpath::stringToPath( trackPath ) );
    STrack *track = dynamic_cast<STrack *>( lane );
    if( !track ) return false;
    return v->groupGesture( track, ungroup );
}

// Resolve an index-path from the root mixer to a track, for the testkit
// entry points below. NULL when the path names no lane.
static STrack *trackAtPath_( const QString &trackPath )
{
    SProject *proj = SApplication::app().getCurrentProject();
    SObject *root = splacements::rootContainer( proj );
    if( !root ) return nullptr;
    SObject *lane = splacements::laneAt( root, strackpath::stringToPath( trackPath ) );
    return dynamic_cast<STrack *>( lane );
}

bool SMainWindow::selectTrackGesture( const QString &trackPath,
                                      Qt::KeyboardModifiers mods )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return false;
    STrack *track = trackAtPath_( trackPath );
    if( !track ) return false;
    return v->tkClickTrackHead( track, mods );
}

bool SMainWindow::toggleTrackHead( const QString &trackPath,
                                   const QString &which, bool on )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return false;
    STrack *track = trackAtPath_( trackPath );
    if( !track ) return false;
    return v->tkToggleTrackHead( track, which, on );
}

bool SMainWindow::dragTrackHead( const QString &trackPath, int targetRow,
                                 bool nestOnto )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return false;
    STrack *track = trackAtPath_( trackPath );
    if( !track ) return false;
    return v->tkDragTrackHead( track, targetRow, nestOnto );
}

QString SMainWindow::describeTrackMeter( const QString &trackPath, int headHeight )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return QString();

    // The APP's current project, not this window's: a headless --test-case run
    // drives SApplication directly and leaves SMainWindow::currentProject_ null.
    SProject *proj = SApplication::app().getCurrentProject();
    if( !proj ) return QString();

    // Path-addressed: the old top-level scan could not describe a nested lane's
    // head at all, so the density rules and the audibility rule had no coverage
    // there.
    SObject *root = splacements::rootContainer( proj );
    SObject *lane = splacements::laneAt( root, strackpath::stringToPath( trackPath ) );
    STrack *track = dynamic_cast<STrack *>( lane );
    if( !track ) return QString();

    // A head built for the assertion and thrown away — parentless and never
    // shown, so no native window appears (a qxa run on Windows uses the real
    // platform plugin). resize() runs the real updateLayout()/applyDensity(),
    // which is the thing under test.
    SSMVMixerControl head( nullptr, *v, *track );
    head.resize( SMV_TRACK_CTRL_WIDTH, headHeight > 0 ? headHeight : 1 );
    return head.describeMeter();
}

// --- proposal 37 P4 test seams -------------------------------------------

QString SMainWindow::describeTrackHead( const QString &trackPath,
                                        int headHeight )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return QString();

    // The APP's project, not this window's: a headless --test-case run drives
    // SApplication directly and leaves currentProject_ null (the same reason
    // describeTrackMeter reads it here).
    SProject *proj = SApplication::app().getCurrentProject();
    if( !proj ) return QString();

    SObject *root = splacements::rootContainer( proj );
    SObject *lane = splacements::laneAt( root, strackpath::stringToPath( trackPath ) );
    STrack *track = dynamic_cast<STrack *>( lane );
    if( !track ) return QString();

    // A head built for the assertion and thrown away - parentless and never
    // shown, so no native window appears. resize() runs the real
    // updateLayout()/applyDensity(), which is the thing under test.
    SSMVMixerControl head( nullptr, *v, *track );
    head.resize( SMV_TRACK_CTRL_WIDTH, headHeight > 0 ? headHeight : 1 );
    return head.describeHead();
}


// --- proposal 37 P6 test seams -------------------------------------------

bool SMainWindow::grabTrackHead( const QString &path, const QString &trackPath,
                                 int headHeight, int w, int h )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return false;
    STrack *track = trackAtPath_( trackPath );
    if( !track ) return false;

    // Same off-screen head describeTrackHead builds - parentless, never shown -
    // so what is painted is the widget under test at the size under test.
    SSMVMixerControl head( nullptr, *v, *track );
    head.resize( w > 0 ? w : SMV_TRACK_CTRL_WIDTH,
                 h > 0 ? h : ( headHeight > 0 ? headHeight : 160 ) );
    if( head.layout() ) head.layout()->activate();
    head.describeHead();          // re-applies the density rules for this size
    const QPixmap pm = head.grab();
    if( pm.isNull() ) return false;
    return pm.save( path, "PNG" );
}

bool SMainWindow::dragAutomationPoint( const QString &owner, const QString &target,
                                       int slotIndex, int take, offset_t time,
                                       double value, offset_t toTime,
                                       double toValue, Qt::KeyboardModifiers mods )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return false;
    return v->dragAutomationPoint( owner, target, slotIndex, take, time, value,
                                   toTime, toValue, mods );
}

bool SMainWindow::showAutomationLane( const QString &trackPath,
                                      const QString &target, int slotIndex,
                                      bool show )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return false;
    STrack *track = trackAtPath_( trackPath );
    if( !track ) return false;
    return v->showAutomationLane( track, target, slotIndex, show );
}

bool SMainWindow::setClipEnvelopeEdit( bool on )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return false;
    v->setClipEnvelopeEdit( on );
    return true;
}

bool SMainWindow::grabArrangerLanes( const QString &path, int w, int h )
{
    SStdMixerView *v = ensureArranger_();
    if( !v || !v->contentView() ) return false;
    SMVActualView *canvas = v->contentView();
    // The canvas is inside a never-shown window, so it has whatever size the
    // hidden layout gave it - 150x240 on this machine, which is not a picture
    // of anything. Its PARENT owns its geometry (the same trap grabEventEditor
    // hit with the dock), so the view has to be sized first and its layout
    // activated; resizing only the child is undone before grab() renders.
    if( w > 0 && h > 0 ) {
        // The canvas is inside a never-shown window, so it has whatever size
        // the hidden layout gave it - 150x240 on this machine, which is not a
        // picture of anything. Its geometry belongs to its parents all the way
        // up (the same trap grabEventEditor hit with the dock), so the WINDOW
        // is what has to be resized; resizing the child alone is undone before
        // grab() renders. The extra room is the scrollbars and the toolbars.
        resize( w + v->getTrackControlWidth() + 48, h + 160 );
        if( layout() ) layout()->activate();
        if( v->layout() ) v->layout()->activate();
    }
    const QPixmap pm = canvas->grab();
    if( pm.isNull() ) return false;
    return pm.save( path, "PNG" );
}

QString SMainWindow::describeEventEditor( const QString &clipPath,
                                          const QString &kind )
{
    if( !eventEditor_ ) return QString();
    // The editor's axis mirrors the arranger's zoom/scroll, so the arranger has
    // to exist before the description (and the PNG) mean anything.
    ensureArranger_();
    linkEventEditorAxis();
    if( !kind.isEmpty() ) eventEditor_->setKind( kind );

    // Empty clipPath = whatever the SELECTION resolves to, which is the
    // production behaviour. A named path binds the dock explicitly, so a case
    // can describe a clip it has not selected.
    eventEditor_->bindClip( strackpath::stringToPath( clipPath ) );
    return eventEditor_->describe();
}

bool SMainWindow::grabEventEditor( const QString &path, int w, int h )
{
    if( !eventEditor_ || !qDockEventEditor_ ) return false;

    // The dock's LAYOUT owns the widget's geometry, so a resize() while it is
    // parented is undone before grab() renders - the first version of this grab
    // produced a 620x186 strip of whatever the hidden main window happened to
    // allot. Detach, size, grab, re-attach: the widget under test is still the
    // real one, at a size a human can read.
    const bool detach = ( w > 0 && h > 0 );
    if( detach ) {
        eventEditor_->setParent( nullptr );
        eventEditor_->resize( w, h );
        if( eventEditor_->layout() ) eventEditor_->layout()->activate();
    }
    const QPixmap pm = eventEditor_->grab();
    if( detach ) qDockEventEditor_->setWidget( eventEditor_ );
    if( pm.isNull() ) return false;
    return pm.save( path, "PNG" );
}

bool SMainWindow::dragNote( const QString &clipPath, qint64 tick, int key,
                            int channel, qint64 toTick, int toKey,
                            const QString &edge, const QString &lane,
                            double toValue )
{
    if( !eventEditor_ ) return false;
    // The axis must exist before a gesture can be expressed in pixels; the
    // arranger owns the zoom the editor mirrors.
    ensureArranger_();
    linkEventEditorAxis();

    eventEditor_->bindClip( strackpath::stringToPath( clipPath ) );
    SPianoRollView *roll = eventEditor_->pianoRoll();
    if( !roll ) return false;
    return roll->tkDragNote( tick, key, channel, toTick, toKey, edge, lane,
                             toValue );
}

bool SMainWindow::virtualKey( int key, double velocity, qint64 durationTicks )
{
    if( !virtualKeys_ ) return false;
    return virtualKeys_->pressNote( key, velocity, durationTicks );
}

bool SMainWindow::grabLevelMeter( const QString &path, double peak, double rms,
                                  bool vertical, int w, int h )
{
    SLevelMeter meter( nullptr );
    meter.setOrientation( vertical ? Qt::Vertical : Qt::Horizontal );
    meter.resize( w, h );

    // Two pushes a simulated second apart: the first sets the peak and arms the
    // hold, the second lets the peak decay away from it so the held tick is drawn
    // in a DIFFERENT place than the bar top — otherwise the grab could not tell
    // the two apart.
    twLevelSample s;
    s.peak       = (float) peak;
    s.meanSquare = (float) ( rms * rms );
    s.frames     = 1024;
    s.clipped    = ( peak >= TW_METER_CLIP_THRESHOLD );
    meter.pushLevel( s, 0 );
    s.peak = (float) ( peak * 0.5 );
    meter.pushLevel( s, 250 );

    const QPixmap pm = meter.grab();
    if( pm.isNull() ) return false;
    return pm.save( path, "PNG" );
}

bool SMainWindow::arrangerSetLaneView( int laneScaleRow, double laneScale,
                                       int toggleTakesRow, int baseTrackHeight,
                                       int topRow )
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return false;
    if( toggleTakesRow >= 0 ) {
        const STrackRow *r = v->rowAt( toggleTakesRow );
        if( !r || !r->track ) return false;
        v->toggleTrackTakesExpanded( r->track );
    }
    if( laneScaleRow >= 0 && laneScale > 0.0 ) {
        const STrackRow *r = v->rowAt( laneScaleRow );
        if( !r || !r->track ) return false;
        v->setTrackHeightScale( r->track, laneScale );
    }
    if( baseTrackHeight > 0 ) v->tkSetBaseTrackHeight( baseTrackHeight );
    if( topRow >= 0 )         v->tkSetTopRow( topRow );
    return true;
}

QString SMainWindow::arrangerLaneAlignment()
{
    SStdMixerView *v = ensureArranger_();
    if( !v ) return QStringLiteral( "no arranger view" );
    return v->tkCheckLaneAlignment();
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

void SMainWindow::onTempoSpinChanged( double bpm )
{
    // Through the set-tempo VERB (proposal 37 D2): it is the only tempo write,
    // it re-derives every beats-timebase link so MIDI clips stay on their bar,
    // and it coalesces a spin-box drag into one undo step. The resulting
    // bpmTempoChanged updates the grid and echoes back to the box, but
    // setValue to an unchanged value emits nothing, so there is no loop.
    if( currentProject_ ) SApplication::app().submitAction( new SSetTempoAction( bpm ) );
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
        auto spk = SApplication::app().getSpeaker();
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

