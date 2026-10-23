#ifndef SACTION_H
#define SACTION_H

#include <QString>
#include <QStringList>
#include <QDomElement>

#include <atomic>

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

    // THE GESTURE TOKEN (QBX-206; QBX-203 built the first one, for notes).
    //
    // A merge key says WHICH control an action touched. It cannot say WHICH
    // GESTURE touched it, and the two merge layers do not agree about that:
    // SActionQueue::enqueue merges only with the PENDING tail, so it is
    // bounded by the drain, but QUndoStack::push folds a new command into the
    // top one whenever SActionUndoCommand::id() matches, with NO time or
    // gesture boundary at all. A key built from a path alone therefore merged
    // every edit that control ever received, for the lifetime of the stack:
    // two separate fader drags became ONE undo step, and because the stack was
    // then one entry short, the undo AFTER them took back the `add-track`
    // before them (measured in control_edit_undo_steps.qxa).
    //
    // UNIQUE BY DEFAULT, which is the safe direction: an action nobody stamped
    // merges with nothing. It is also why no discrete control needs wiring --
    // a dialog, a combo box, a wheel notch, a double-click reset or a scripted
    // verb is one committed value and therefore one undo step, which is what
    // each of them already meant. Only a control that STREAMS through one
    // gesture has anything to say: it mints a token on press and stamps every
    // action it submits until release.
    static quint64 newGesture()
    {
        static std::atomic<quint64> next{ 0 };
        return ++next;              // never 0, so 0 can mean "no gesture"
    }
    quint64 gesture() const { return gesture_; }
    // Where a `.qxa`'s own `gesture="n"` numbers live (QBX-206). A script
    // needs to be able to say "these two elements are one gesture", and its
    // small integers must never collide with a token newGesture() handed a
    // real drag -- so they are offset into a range the counter cannot reach.
    static constexpr quint64 SCRIPT_GESTURE_BASE = 1ull << 56;
    // 0 is IGNORED on purpose, so a control may stamp unconditionally with
    // whatever it is holding and a slider nobody pressed keeps its unique
    // default instead of being lumped in with the last gesture.
    void setGesture( quint64 g ) { if( g ) gesture_ = g; }

    // Opt-in: this action, once APPLIED, marks the undo stack CLEAN — the file
    // on disk now matches what is in memory. `save-project` is the only
    // override today. It is a hook here rather than a `name() == "save-
    // project"` string check in SActionHistory because that is exactly the
    // kind of coupling `mergeKey()`/`knownAttributes()` already avoid: the
    // history owns the QUndoStack and applies every action the SAME way,
    // scripted or interactive, so this is the one place both a `.qxa`
    // `<save-project>` verb and the menu's Save route through and both need
    // to say the same thing. Default false — everything else is a project
    // MUTATION, not a write of it to disk.
    virtual bool marksProjectClean() const { return false; }

protected:
    // The two halves of the gesture boundary, for an implementation that has a
    // mergeKey(). BOTH are required: QBX-203 measured that reverting either
    // one alone leaves every gate passing, because each fully prevents the
    // merge on its own. A single-site regression must not be able to restore
    // the old behaviour silently.
    QString gestureKeySuffix() const
    { return QStringLiteral( ":g%1" ).arg( gesture_ ); }
    bool sameGesture( const SAction *other ) const
    { return other && other->gesture_ == gesture_; }

    QString pathRoot_;      // see pathRoot(); empty == the master root
    quint64 gesture_ = newGesture();   // see newGesture(); unique by default
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
