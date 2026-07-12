#ifndef SACTIONRUNNER_H
#define SACTIONRUNNER_H

#include <QString>
#include <QStringList>
#include "app/testkit/sactionscript.h"

class SApplication;
class SProject;

// Executes an action script against a project and returns results.
// Phase 1: script load + execute.
// Phase 2: assertions (track-count, project-matches, verify-undo).
class SActionRunner {
public:
    struct Result {
        bool       passed = false;
        int        actionsApplied = 0;
        int        actionsRejected = 0;
        int        assertionsFailed = 0;
        QStringList failures;   // human-readable rejection/assertion reasons
        QStringList artifacts;  // paths to generated files (screenshots, renders, etc.)
    };

    // Execute a parsed script against a project.
    // Creates the project per script setup, submits actions via SActionHistory,
    // evaluates assertions, handles undo verification.
    Result run(const SActionScript &script, SApplication &app);

private:
    // Evaluate assertions against the project. Returns true if all pass.
    bool evaluateAssertions_(const SActionScript &script, SProject *project,
                             QStringList &failures) const;

    // Assertion evaluators: each returns true if assertion passes.
    bool assertTrackCount_(const SActionScript::Assertion &a, SProject *project,
                           QStringList &failures) const;
    bool assertProjectMatches_(const SActionScript::Assertion &a, SProject *project,
                               QStringList &failures) const;

    // Undo/redo verification: replay inverses and re-forward, assert state matches.
    bool verifyUndo_(SProject *project, SApplication &app, QStringList &failures) const;
};

#endif // SACTIONRUNNER_H
