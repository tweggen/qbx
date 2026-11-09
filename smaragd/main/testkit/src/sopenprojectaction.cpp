#include "app/testkit/sopenprojectaction.h"

#include <QCoreApplication>
#include <QDomElement>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QUndoStack>

#include "app/actions/sactionhistory.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sproject.h"
#include "app/persistence/sloadprojectaction.h"
#include "app/shell/sapplication.h"
#include "tw/core/twlog.h"

SApplyResult SOpenProjectAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };
    // Refused BEFORE anything is torn down: a typo must not cost the case its
    // project.
    const QString path = QFileInfo( path_ ).absoluteFilePath();
    if( path_.isEmpty() || !QFileInfo::exists( path ) ) {
        TW_LOGW( "ui.testkit", "[OPEN-PROJECT] no such file '%s'",
                 path_.toStdString().c_str() );
        return { false, nullptr };
    }

    // Deferred, as close-project is: drain_() still holds `project`.
    QPointer<SProject> old( project );
    QMetaObject::invokeMethod( QCoreApplication::instance(), [old, path]() {
        SApplication &app = SApplication::app();
        if( !old || app.getCurrentProject() != old.data() ) return;

        // SMainWindow::closeProject()'s order, as close-project does it.
        app.cancelRecordPreamble();
        if( app.isRecordingActive() ) app.stopRecording();
        if( app.isPlaying() ) app.setPlaybackRunning( false );

        const QString baseDir = old->sampleBaseDir();
        app.setCurrentProject( nullptr );
        app.rewireSpeaker();
        if( SActionHistory *history = app.actionHistory() )
            history->undoStack()->clear();
        delete old.data();

        // SMainWindow::openProjectFile(): an EMPTY project made current, the
        // load applied to it, then setCurrentProject() AGAIN -- the second
        // call is the one whose projectChanged() sees what the file armed.
        SProject *opened = new SProject();
        opened->setSampleBaseDir( baseDir );
        app.setCurrentProject( opened );
        SLoadProjectAction load( path );
        const SApplyResult r = load.apply( opened );
        if( !r.applied ) {
            TW_LOGE( "ui.testkit", "[OPEN-PROJECT] could not load '%s'; the project "
                                   "is left empty", path.toStdString().c_str() );
            return;
        }
        delete r.inverse;
        app.setCurrentProject( opened );
        app.rewireSpeaker();
        TW_LOGI( "ui.testkit", "[OPEN-PROJECT] opened '%s'", path.toStdString().c_str() );
    }, Qt::QueuedConnection );
    return { true, nullptr };
}

void SOpenProjectAction::writeXml( QDomElement &elem ) const
{
    elem.setAttribute( "path", path_ );
}

bool SOpenProjectAction::readXml( const QDomElement &elem, int )
{
    path_ = elem.attribute( "path" );
    return true;
}

static const bool s_reg_open_project = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "open-project" ),
        []{ return new SOpenProjectAction; } ), true );
