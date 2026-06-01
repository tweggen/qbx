#ifndef SACTION_H
#define SACTION_H

#include <QString>
#include <QDomElement>

class SProject;
class SAction;

// Result of applying an action: did it succeed, and what's the inverse?
struct SApplyResult {
    bool      applied;   // was the precondition met and mutation successful?
    SAction  *inverse;   // ownership transferred to caller; null = not undoable
};

// Base class for all actions. Immutable after construction; forward parameters only.
// Inverse is synthesized at apply time from pre-mutation state.
class SAction {
public:
    virtual ~SAction() = default;

    // Stable identity used as XML tag, scripting verb, and registry key.
    virtual QString name() const = 0;

    // Versioning hook for future format changes. Bumped when writeXml changes.
    virtual int formatVersion() const { return 1; }

    // Engine thread: mutate the project AND construct the inverse from the
    // pre-mutation state observed here.
    // applied=false → precondition failed (target gone, validation error, etc.).
    // Inverse may be null even on applied=true for non-undoable actions.
    virtual SApplyResult apply(SProject *project) = 0;

    // Serialization: forward parameters only.
    virtual void writeXml(QDomElement &elem) const = 0;
    virtual bool readXml(const QDomElement &elem, int version) = 0;

    // Coalescing at enqueue time: same mergeKey() + successful mergeWith()
    // collapses two consecutive actions before the engine sees either.
    virtual QString mergeKey() const { return QString(); }
    virtual bool mergeWith(const SAction * /*later*/) { return false; }
};

#endif // SACTION_H
