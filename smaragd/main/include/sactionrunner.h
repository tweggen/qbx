#ifndef SACTIONRUNNER_H
#define SACTIONRUNNER_H

#include <QString>
#include <QStringList>

class SActionScript;
class SApplication;
class SProject;

// Executes an action script against a project and returns results.
// Phase 1: script load + execute (no assertions yet; those come in Phase 2).
class SActionRunner {
public:
    struct Result {
        bool       passed = false;
        int        actionsApplied = 0;
        int        actionsRejected = 0;
        QStringList failures;  // human-readable rejection reasons
    };

    // Execute a parsed script against a project.
    // Creates the project per script setup, submits actions via SActionHistory,
    // synchronizes via engine queue drain after each action.
    Result run(const SActionScript &script, SApplication &app);
};

#endif // SACTIONRUNNER_H
