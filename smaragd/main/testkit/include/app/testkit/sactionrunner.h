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
    //
    // `teardownProject` (default true, matching the historical behavior):
    // when true, the project is detached from the app and deleted before
    // returning -- the proposal 27 M2 fix, load-bearing for --test-case (see
    // the comment at the teardown site). When false, the project is left
    // attached as the app's current project so a `--run-actions` caller can
    // keep showing it after the script finishes; the caller then owns its
    // eventual teardown (main.cpp rides SMainWindow's normal project-close
    // path for that).
    // Non-const: every action the script parsed is handed to
    // SApplication::submitAction() (ownership transfer -- see
    // SActionScript::releaseActions()'s doc comment), and this call releases
    // the script's own pointers to them once that is done.
    Result run(SActionScript &script, SApplication &app,
               bool teardownProject = true);

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
