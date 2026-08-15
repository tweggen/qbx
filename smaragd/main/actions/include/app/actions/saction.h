#ifndef SACTION_H
#define SACTION_H

#include <QString>
#include <QStringList>
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

    // OPT-IN strict attributes for the .qxa runner.
    //
    // readXml() reads the attributes it knows and silently ignores the rest, so
    // a typo in a case file — `position=` where the verb wanted `timePos=`, an
    // attribute that never existed — costs the case its intent and says
    // nothing: the action applies with its DEFAULTS and the case passes while
    // testing something else. That failure mode has already been paid for once
    // here (split_plain_screenshot's inert `<toggle-playback/>`).
    //
    // A verb that returns a NON-EMPTY list is declaring "these are all of my
    // attributes"; the runner then warns about any other attribute in the XML,
    // and fails the case when SMARAGD_STRICT_ATTRS=1. The default empty list
    // means "undeclared" — checked for nothing, exactly as before. Opt-in
    // because auditing every verb in the suite is a separate job from adding
    // the mechanism.
    virtual QStringList knownAttributes() const { return {}; }

    // Coalescing at enqueue time: same mergeKey() + successful mergeWith()
    // collapses two consecutive actions before the engine sees either.
    virtual QString mergeKey() const { return QString(); }
    virtual bool mergeWith(const SAction * /*later*/) { return false; }
};

#endif // SACTION_H
