#include "sactionrunner.h"
#include "sactionscript.h"
#include "saction.h"
#include "sapplication.h"
#include "sproject.h"
#include "sactionhistory.h"
#include "sstdmixer.h"
#include "strack.h"
#include "slink.h"
#include <QCoreApplication>
#include <QDomDocument>
#include <QFile>

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

    // Initialize with a root mixer (standard for new projects).
    project->setRootComponent(new SStdMixer(project));

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

    // Step 3: Evaluate assertions (Phase 2).
    if (!script.assertions().isEmpty()) {
        if (!evaluateAssertions_(script, project, result.failures)) {
            result.passed = false;
            result.assertionsFailed = result.failures.size();
        }
    }

    result.passed = result.passed && (result.actionsRejected == 0) && (result.assertionsFailed == 0);
    return result;
}

bool SActionRunner::evaluateAssertions_(const SActionScript &script, SProject *project,
                                        QStringList &failures) const
{
    bool allPass = true;

    for (const SActionScript::Assertion &assertion : script.assertions()) {
        bool pass = false;

        if (assertion.kind == "assert-track-count") {
            pass = assertTrackCount_(assertion, project, failures);
        } else if (assertion.kind == "assert-project-matches") {
            pass = assertProjectMatches_(assertion, project, failures);
        } else {
            failures.append(QString("Unknown assertion: %1").arg(assertion.kind));
            pass = false;
        }

        allPass = allPass && pass;
    }

    return allPass;
}

bool SActionRunner::assertTrackCount_(const SActionScript::Assertion &a, SProject *project,
                                      QStringList &failures) const
{
    if (!project) {
        failures.append("assert-track-count: no project");
        return false;
    }

    // Get the root mixer and count tracks.
    SObject *root = project->getRootComponent();
    SStdMixer *mixer = dynamic_cast<SStdMixer*>(root);
    if (!mixer) {
        failures.append("assert-track-count: root is not a mixer");
        return false;
    }

    int actual = mixer->getNTracks();
    int expected = a.args.value("equals", "-1").toInt();

    if (actual != expected) {
        failures.append(QString("assert-track-count: expected %1, got %2")
            .arg(expected).arg(actual));
        return false;
    }

    return true;
}

bool SActionRunner::assertProjectMatches_(const SActionScript::Assertion &a, SProject *project,
                                          QStringList &failures) const
{
    // Phase 2b: Full project XML comparison. For Phase 2, defer this assertion.
    // The golden file path is provided but not yet compared.
    // This is a placeholder for future implementation.
    (void)project;
    (void)a;
    (void)failures;

    failures.append("assert-project-matches: not yet implemented in Phase 2");
    return false;
}
