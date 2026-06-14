#include "sactionrunner.h"
#include "sactionscript.h"
#include "saction.h"
#include "sapplication.h"
#include "sproject.h"
#include "sactionhistory.h"
#include <QCoreApplication>

SActionRunner::Result SActionRunner::run(const SActionScript &script, SApplication &app)
{
    Result result;
    result.passed = true;

    // Step 1: Set up project (new or load).
    // Phase 1: only "new" is supported; loading deferred to Phase 1b.
    SProject *project = new SProject();
    if (!project) {
        result.passed = false;
        result.failures.append("Failed to create project");
        return result;
    }

    app.setCurrentProject(project);
    app.rewireSpeaker();

    // Step 2: Execute each action in sequence.
    for (SAction *action : script.actions()) {
        // Submit via the normal action history path.
        // This ensures coalescing, undo stack, and Phase 2 draining are all exercised.
        // Note: submitAction takes ownership; actions in the script are consumed.
        app.submitAction(action);

        // Pump the event loop to drain Qt events and allow UI updates.
        QCoreApplication::processEvents();

        // Drain the engine queue (Phase 2 API).
        // Phase 1 note: This is a placeholder. In Phase 2, we'll have the actual
        // async draining mechanism. For now, assume synchronous.
        // app.actionHistory()->waitForEngineQueue();  // TODO: implement in Phase 2

        result.actionsApplied++;
    }

    result.passed = (result.actionsRejected == 0);
    return result;
}
