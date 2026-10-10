#ifndef _SCLOSEPROJECTACTION_H_
#define _SCLOSEPROJECTACTION_H_

#include "app/actions/saction.h"

/**
 * `close-project` — close the current project and carry on in a FRESH, empty
 * one, the way File -> Close followed by File -> New would.
 *
 * WHY IT EXISTS: no verb closed or replaced a project, so nothing in a script
 * could reach the one transition the live monitor got wrong. Closing a project
 * while a track was armed left the monitor holding the closed project's
 * tracks, and its 40 ms demand tick then read them after they were freed
 * (SIGSEGV in appendTrackSignature). A gate for that needs a project to die
 * mid-script while the app lives on.
 *
 * THE SWAP IS DEFERRED, and that is not a nicety. apply() runs inside
 * SActionHistory::drain_(), which holds the project in a local and calls
 * notifyArrangementChanged() on it after apply() returns - deleting it here
 * would hand drain_ a freed pointer. So apply() only queues the swap; the
 * runner's processEvents() after the action delivers it, before the next
 * action starts. The runner re-reads the current project after every action
 * for the same reason.
 *
 * The sequence mirrors SMainWindow::closeProject(): stop a count-in, a take
 * and playback, detach (setCurrentProject(nullptr)), drop the undo history
 * while the objects its commands point into are alive, delete; then a new
 * project built as SActionRunner builds the first one. Not undoable.
 */
class SCloseProjectAction : public SAction
{
public:
    SCloseProjectAction() {}
    SApplyResult apply( SProject *project ) override;
    QString name() const override
    { return QStringLiteral( "close-project" ); }
    QStringList knownAttributes() const override { return {}; }
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
};

#endif // _SCLOSEPROJECTACTION_H_
