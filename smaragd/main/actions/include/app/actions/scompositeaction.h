#ifndef SCOMPOSITEACTION_H
#define SCOMPOSITEACTION_H

#include "app/actions/saction.h"
#include <QList>

// An ordered list of actions applied as ONE action (proposal 17). apply()
// runs the children in order; if child i fails, the already-applied
// children are rolled back (their inverses, in reverse) and the whole
// composite reports failure. On success the inverse is the reversed list
// of the children's inverses — so a composite undoes atomically.
//
// Used by planner verbs (place-recording) and the group broadcast
// (phase 4). Children are owned by the composite. Non-undoable children
// (null inverse) make the whole composite non-undoable — rollback of later
// failures then covers only the invertible prefix (best effort, logged by
// the caller if it cares).
//
// Live-only: not registered as a verb; scripts compose primitives instead.
class SCompositeAction : public SAction {
public:
    SCompositeAction() = default;
    explicit SCompositeAction( const QList<SAction *> &children );
    ~SCompositeAction() override;

    void append( SAction *child ) { children_.append( child ); }
    int count() const { return children_.size(); }

    QString name() const override { return QStringLiteral("composite"); }
    SApplyResult apply( SProject *project ) override;
    void writeXml( QDomElement &elem ) const override;
    bool readXml( const QDomElement &elem, int version ) override;

private:
    QList<SAction *> children_;
};

#endif // SCOMPOSITEACTION_H
