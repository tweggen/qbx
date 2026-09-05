#ifndef SREMOVEARRANGEMENTACTION_H
#define SREMOVEARRANGEMENTACTION_H

#include "app/actions/saction.h"
#include <QString>

class SObject;

// Action: unregister a named arrangement root, dropping the registry's pin.
//
// REFUSED while any registered asset still windows that root. The refusal is
// the interesting part: unregistering would drop the only reference a detached
// root has, and an asset windowing a dead container is not a recoverable state
// — its undo could not rebuild the arrangement's contents. An asset must be
// removed first (or the whole thing dissolved), which is exactly what
// dissolve-arrangement will do as one macro.
//
// Inverse: SRestoreArrangementAction, which re-registers THE SAME root — this
// action PINS it (SRemoveTrackAction's pattern) so it survives the removal.
//
// IT USED TO BE SCreateArrangementAction, WHICH BUILDS A FRESH EMPTY ROOT, and
// the argument for that was: "correct only because the refusal above means a
// removable arrangement is one nothing references". THAT ARGUMENT HAD A HOLE
// THE MOMENT PROPOSAL 45 M1 LANDED. The `childCount() > 0` refusal in apply()
// exists precisely so undo cannot lose lanes — but a MASTER LANE is not a
// child link (D2), so childCount() never saw it, and a master with inserts, a
// fader and automation lanes came back from undo blank. Measured: plugins=0,
// volume=0. Latent while the master lane was in no signal path; audible state
// since M2.
//
// Widening the refusal to "and the master lane must be untouched" was
// considered and rejected: it makes a perfectly ordinary undo impossible
// instead of correct, and it would need widening again for every future
// system lane. Pinning restores what was there, which is what undo means.
class SRemoveArrangementAction : public SAction {
public:
    SRemoveArrangementAction() = default;
    explicit SRemoveArrangementAction( const QString &name );
    ~SRemoveArrangementAction() override;

    QString name() const override { return QStringLiteral("remove-arrangement"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;
    QStringList knownAttributes() const override { return { QStringLiteral("name") }; }

    /** The pinned root, for the inverse. Null before the first apply. */
    SObject *heldRoot() const { return heldRoot_; }
    /** Called by the inverse once it has handed the root back to the project:
     * the registry takes its own ref, so ours must go or the arrangement can
     * never be destroyed. */
    void releaseHeld();

private:
    void dropStalePin();

    QString  name_;
    SObject *heldRoot_ = nullptr;   // pinned across undo/redo, not owned
    bool     holdsRef_ = false;
};

// The inverse half. Not a scriptable verb -- it exists only on the undo stack,
// and it addresses its root by POINTER (through the action that pinned it)
// rather than by name, which is the whole point: a name would rebuild an empty
// mixer and lose the master lane's state again.
class SRestoreArrangementAction : public SAction {
public:
    SRestoreArrangementAction( SRemoveArrangementAction *owner,
                               const QString &name );

    QString name() const override { return QStringLiteral("restore-arrangement"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    SRemoveArrangementAction *owner_;   // holds the pinned root (not owned)
    QString name_;
};

#endif // SREMOVEARRANGEMENTACTION_H
