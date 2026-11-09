#ifndef _SOPENPROJECTACTION_H_
#define _SOPENPROJECTACTION_H_

#include "app/actions/saction.h"

/**
 * `open-project path=` — close the current project and open a saved one the
 * way File -> Open does (QBX-291).
 *
 * WHY IT EXISTS: `load-project` loads INTO the running project and never runs
 * SApplication's project switch, so everything that switch does is invisible
 * to a script -- above all SLiveMonitor::projectChanged(), which marks every
 * track that arrived ARMED as inert (design D9). A gate for an inert arm needs
 * the real path.
 *
 * The sequence is close-project's (stop a count-in, a take and playback,
 * detach, clear the undo history, delete), then SMainWindow::openProjectFile's:
 * an empty project made current, SLoadProjectAction applied to it, and
 * setCurrentProject() called AGAIN on the loaded project -- the second call is
 * the one that sees the armed tracks. DEFERRED for the same reason as
 * close-project (drain_ still holds the old project when apply() returns);
 * the runner's processEvents() after the action delivers it.
 *
 * REJECTED when the file does not exist (checked before anything is torn
 * down). A load that fails after that leaves an empty project and logs an
 * error. Not undoable.
 */
class SOpenProjectAction : public SAction
{
public:
    SOpenProjectAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "open-project" ); }
    QStringList knownAttributes() const override { return { QStringLiteral( "path" ) }; }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QString path_;
};

#endif // _SOPENPROJECTACTION_H_
