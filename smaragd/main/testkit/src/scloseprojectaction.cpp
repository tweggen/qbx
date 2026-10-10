#include "app/testkit/scloseprojectaction.h"

#include <QCoreApplication>
#include <QDomElement>
#include <QMetaObject>
#include <QPointer>
#include <QUndoStack>

#include "app/actions/sactionhistory.h"
#include "app/actions/sactionregistry.h"
#include "app/model/sproject.h"
#include "app/objects/mixer/sstdmixer.h"
#include "app/shell/sapplication.h"
#include "tw/core/twlog.h"

SApplyResult SCloseProjectAction::apply( SProject *project )
{
    if( !project ) return { false, nullptr };

    // See the header: drain_() still holds `project` when this returns, so
    // the swap runs on the next turn of the event loop, not here.
    QPointer<SProject> old( project );
    QMetaObject::invokeMethod( QCoreApplication::instance(), [old]() {
        SApplication &app = SApplication::app();
        if( !old || app.getCurrentProject() != old.data() ) return;

        // What SMainWindow::closeProject() does first: nothing that is still
        // writing into the project - a count-in, a take, the transport - may
        // outlive it.
        app.cancelRecordPreamble();
        if( app.isRecordingActive() ) app.stopRecording();
        if( app.isPlaying() ) app.setPlaybackRunning( false );

        const QString baseDir = old->sampleBaseDir();
        app.setCurrentProject( nullptr );
        app.rewireSpeaker();
        if( SActionHistory *history = app.actionHistory() )
            history->undoStack()->clear();
        delete old.data();

        // A fresh project, exactly as SActionRunner::run() builds the first.
        SProject *fresh = new SProject();
        fresh->setSampleBaseDir( baseDir );
        fresh->setRootComponent( new SStdMixer( fresh ) );
        app.setCurrentProject( fresh );
        app.rewireSpeaker();
        TW_LOGI( "ui.testkit", "[CLOSE-PROJECT] closed the project, opened a fresh one" );
    }, Qt::QueuedConnection );
    return { true, nullptr };
}

void SCloseProjectAction::writeXml( QDomElement & ) const
{
}

bool SCloseProjectAction::readXml( const QDomElement &, int )
{
    return true;
}

static const bool s_reg_close_project = (
    SActionRegistry::instance().registerType(
        QStringLiteral( "close-project" ),
        []{ return new SCloseProjectAction; } ), true );
