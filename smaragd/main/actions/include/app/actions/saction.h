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

    // --- The ROOT this action's paths address (proposal 09 D21) -----------
    //
    // A project has more than one summing root: the MASTER, plus any number of
    // named ARRANGEMENTs. An index path alone cannot say which one it means,
    // and index {0} usually exists in ALL of them -- so a path resolved
    // against the wrong root SUCCEEDS and edits the wrong tree. That is silent
    // corruption, not a failed action, which is why the root travels WITH the
    // action rather than as ambient state.
    //
    // Empty == the master, which is what every action written before this
    // means and what a bare "0,1" still parses to. It lives on the BASE
    // because every path-taking action needs exactly one of them and because
    // an INVERSE must inherit its forward action's root -- a rule a checker
    // can then enforce in one place (tools/check_pathroot.py).
    const QString &pathRoot() const { return pathRoot_; }
    void setPathRoot( const QString &root ) { pathRoot_ = root; }

    // Coalescing at enqueue time: same mergeKey() + successful mergeWith()
    // collapses two consecutive actions before the engine sees either.
    //
    // NOTE (D21): a merge key is a PATH, and two faders at index {0} in two
    // different roots have the same one. Implementations that build a key from
    // a path must include pathRoot() or the two merge into one undo entry that
    // then restores the wrong root's value.
    virtual QString mergeKey() const { return QString(); }
    virtual bool mergeWith(const SAction * /*later*/) { return false; }

protected:
    QString pathRoot_;      // see pathRoot(); empty == the master root
};


// Apply `a` and hand its INVERSE the same path root (proposal 09 D21).
//
// AN INVERSE ACTS ON THE SAME OBJECTS AS ITS FORWARD ACTION, so it addresses
// the same root -- always, with no exception any action gets to opt out of.
// Doing it HERE rather than at each `new S…Action(…)` is not a shortcut: there
// are 83 such sites across 36 files, every one of them constructs its inverse
// with a bare index path, and a single one forgotten is an undo that silently
// edits the MASTER instead of the arrangement the edit was made in. A funnel
// cannot be forgotten.
//
// Both funnels call this: SActionHistory (submit/undo/redo) and
// SCompositeAction (its members, whose inverses never reach the history
// individually).
inline SApplyResult applyPropagatingRoot( SAction *a, SProject *project )
{
    SApplyResult r = a->apply( project );
    if( r.inverse ) r.inverse->setPathRoot( a->pathRoot() );
    return r;
}

#endif // SACTION_H
